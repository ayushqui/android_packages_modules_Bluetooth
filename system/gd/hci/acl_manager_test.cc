/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hci/acl_manager.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <map>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/bind.h"
#include "hci/address.h"
#include "hci/controller.h"
#include "hci/hci_layer.h"
#include "os/thread.h"
#include "packet/raw_builder.h"

namespace bluetooth {
namespace hci {
namespace {

using common::BidiQueue;
using common::BidiQueueEnd;
using packet::kLittleEndian;
using packet::PacketView;
using packet::RawBuilder;

constexpr std::chrono::seconds kTimeout = std::chrono::seconds(2);

PacketView<kLittleEndian> GetPacketView(std::unique_ptr<packet::BasePacketBuilder> packet) {
  auto bytes = std::make_shared<std::vector<uint8_t>>();
  BitInserter i(*bytes);
  bytes->reserve(packet->size());
  packet->Serialize(i);
  return packet::PacketView<packet::kLittleEndian>(bytes);
}

std::unique_ptr<BasePacketBuilder> NextPayload(uint16_t handle) {
  static uint32_t packet_number = 1;
  auto payload = std::make_unique<RawBuilder>();
  payload->AddOctets2(handle);
  payload->AddOctets4(packet_number++);
  return std::move(payload);
}

std::unique_ptr<AclPacketBuilder> NextAclPacket(uint16_t handle) {
  PacketBoundaryFlag packet_boundary_flag = PacketBoundaryFlag::FIRST_AUTOMATICALLY_FLUSHABLE;
  BroadcastFlag broadcast_flag = BroadcastFlag::ACTIVE_SLAVE_BROADCAST;
  return AclPacketBuilder::Create(handle, packet_boundary_flag, broadcast_flag, NextPayload(handle));
}

class TestController : public Controller {
 public:
  void RegisterCompletedAclPacketsCallback(common::Callback<void(uint16_t /* handle */, uint16_t /* packets */)> cb,
                                           os::Handler* handler) override {
    acl_cb_ = cb;
    acl_cb_handler_ = handler;
  }

  uint16_t GetControllerAclPacketLength() override {
    return acl_buffer_length_;
  }

  uint16_t GetControllerNumAclPacketBuffers() override {
    return total_acl_buffers_;
  }

  void CompletePackets(uint16_t handle, uint16_t packets) {
    acl_cb_handler_->Post(common::BindOnce(acl_cb_, handle, packets));
  }

  uint16_t acl_buffer_length_ = 1024;
  uint16_t total_acl_buffers_ = 2;
  common::Callback<void(uint16_t /* handle */, uint16_t /* packets */)> acl_cb_;
  os::Handler* acl_cb_handler_ = nullptr;

 protected:
  void Start() override {}
  void Stop() override {}
  void ListDependencies(ModuleList* list) override {}
};

class TestHciLayer : public HciLayer {
 public:
  void EnqueueCommand(std::unique_ptr<CommandPacketBuilder> command,
                      common::OnceCallback<void(CommandStatusView)> on_status, os::Handler* handler) override {
    command_queue_.push(std::move(command));
    not_empty_.notify_all();
  }

  void EnqueueCommand(std::unique_ptr<CommandPacketBuilder> command,
                      common::OnceCallback<void(CommandCompleteView)> on_complete, os::Handler* handler) override {
    command_queue_.push(std::move(command));
    command_complete_callbacks.push_front(std::move(on_complete));
    not_empty_.notify_all();
  }

  std::unique_ptr<CommandPacketBuilder> GetLastCommand() {
    if (command_queue_.size() == 0) {
      return nullptr;
    }
    auto last = std::move(command_queue_.front());
    command_queue_.pop();
    return last;
  }

  ConnectionManagementCommandView GetCommandPacket(OpCode op_code) {
    std::unique_lock<std::mutex> lock(mutex_);
    std::chrono::milliseconds time = std::chrono::milliseconds(3000);

    ASSERT(not_empty_.wait_for(lock, time) != std::cv_status::timeout);
    auto packet = GetLastCommand();
    auto packet_view = GetPacketView(std::move(packet));
    CommandPacketView command_packet_view = CommandPacketView::Create(packet_view);
    ConnectionManagementCommandView command = ConnectionManagementCommandView::Create(command_packet_view);
    ASSERT(command.IsValid());
    EXPECT_EQ(command.GetOpCode(), op_code);

    return command;
  }

  void RegisterEventHandler(EventCode event_code, common::Callback<void(EventPacketView)> event_handler,
                            os::Handler* handler) override {
    registered_events_[event_code] = event_handler;
  }

  void UnregisterEventHandler(EventCode event_code) override {
    registered_events_.erase(event_code);
  }

  void IncomingEvent(std::unique_ptr<EventPacketBuilder> event_builder) {
    auto packet = GetPacketView(std::move(event_builder));
    EventPacketView event = EventPacketView::Create(packet);
    EXPECT_TRUE(event.IsValid());
    EventCode event_code = event.GetEventCode();
    EXPECT_TRUE(registered_events_.find(event_code) != registered_events_.end());
    registered_events_[event_code].Run(event);
  }

  void IncomingAclData(uint16_t handle) {
    os::Handler* hci_handler = GetHandler();
    auto* queue_end = acl_queue_.GetDownEnd();
    std::promise<void> promise;
    auto future = promise.get_future();
    queue_end->RegisterEnqueue(hci_handler,
                               common::Bind(
                                   [](decltype(queue_end) queue_end, uint16_t handle, std::promise<void> promise) {
                                     auto packet = GetPacketView(NextAclPacket(handle));
                                     AclPacketView acl2 = AclPacketView::Create(packet);
                                     queue_end->UnregisterEnqueue();
                                     promise.set_value();
                                     return std::make_unique<AclPacketView>(acl2);
                                   },
                                   queue_end, handle, common::Passed(std::move(promise))));
    auto status = future.wait_for(kTimeout);
    ASSERT_EQ(status, std::future_status::ready);
  }

  void AssertNoOutgoingAclData() {
    auto queue_end = acl_queue_.GetDownEnd();
    EXPECT_EQ(queue_end->TryDequeue(), nullptr);
  }

  void CommandCompleteCallback(EventPacketView event) {
    CommandCompleteView complete_view = CommandCompleteView::Create(event);
    ASSERT(complete_view.IsValid());
    std::move(command_complete_callbacks.front()).Run(complete_view);
    command_complete_callbacks.pop_front();
  }

  PacketView<kLittleEndian> OutgoingAclData() {
    auto queue_end = acl_queue_.GetDownEnd();
    std::unique_ptr<AclPacketBuilder> received;
    do {
      received = queue_end->TryDequeue();
    } while (received == nullptr);

    return GetPacketView(std::move(received));
  }

  BidiQueueEnd<AclPacketBuilder, AclPacketView>* GetAclQueueEnd() override {
    return acl_queue_.GetUpEnd();
  }

  void ListDependencies(ModuleList* list) override {}
  void Start() override {}
  void Stop() override {}

 private:
  std::map<EventCode, common::Callback<void(EventPacketView)>> registered_events_;
  std::list<base::OnceCallback<void(CommandCompleteView)>> command_complete_callbacks;
  BidiQueue<AclPacketView, AclPacketBuilder> acl_queue_{3 /* TODO: Set queue depth */};

  std::queue<std::unique_ptr<CommandPacketBuilder>> command_queue_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
};

class AclManagerNoCallbacksTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_hci_layer_ = new TestHciLayer;  // Ownership is transferred to registry
    test_controller_ = new TestController;
    fake_registry_.InjectTestModule(&HciLayer::Factory, test_hci_layer_);
    fake_registry_.InjectTestModule(&Controller::Factory, test_controller_);
    client_handler_ = fake_registry_.GetTestModuleHandler(&HciLayer::Factory);
    EXPECT_NE(client_handler_, nullptr);
    fake_registry_.Start<AclManager>(&thread_);
    acl_manager_ = static_cast<AclManager*>(fake_registry_.GetModuleUnderTest(&AclManager::Factory));
    Address::FromString("A1:A2:A3:A4:A5:A6", remote);
  }

  void TearDown() override {
    fake_registry_.SynchronizeModuleHandler(&AclManager::Factory, std::chrono::milliseconds(20));
    fake_registry_.StopAll();
  }

  TestModuleRegistry fake_registry_;
  TestHciLayer* test_hci_layer_ = nullptr;
  TestController* test_controller_ = nullptr;
  os::Thread& thread_ = fake_registry_.GetTestThread();
  AclManager* acl_manager_ = nullptr;
  os::Handler* client_handler_ = nullptr;
  Address remote;

  std::future<void> GetConnectionFuture() {
    ASSERT_LOG(mock_connection_callback_.connection_promise_ == nullptr, "Promises promises ... Only one at a time");
    mock_connection_callback_.connection_promise_ = std::make_unique<std::promise<void>>();
    return mock_connection_callback_.connection_promise_->get_future();
  }

  std::shared_ptr<AclConnection> GetLastConnection() {
    return mock_connection_callback_.connections_.back();
  }

  void SendAclData(uint16_t handle, std::shared_ptr<AclConnection> connection) {
    auto queue_end = connection->GetAclQueueEnd();
    std::promise<void> promise;
    auto future = promise.get_future();
    queue_end->RegisterEnqueue(client_handler_,
                               common::Bind(
                                   [](decltype(queue_end) queue_end, uint16_t handle, std::promise<void> promise) {
                                     queue_end->UnregisterEnqueue();
                                     promise.set_value();
                                     return NextPayload(handle);
                                   },
                                   queue_end, handle, common::Passed(std::move(promise))));
    auto status = future.wait_for(kTimeout);
    ASSERT_EQ(status, std::future_status::ready);
  }

  class MockConnectionCallback : public ConnectionCallbacks {
   public:
    void OnConnectSuccess(std::unique_ptr<AclConnection> connection) override {
      // Convert to std::shared_ptr during push_back()
      connections_.push_back(std::move(connection));
      if (connection_promise_ != nullptr) {
        connection_promise_->set_value();
        connection_promise_.reset();
      }
    }
    MOCK_METHOD2(OnConnectFail, void(Address, ErrorCode reason));

    std::list<std::shared_ptr<AclConnection>> connections_;
    std::unique_ptr<std::promise<void>> connection_promise_;
  } mock_connection_callback_;
};

class AclManagerTest : public AclManagerNoCallbacksTest {
 protected:
  void SetUp() override {
    AclManagerNoCallbacksTest::SetUp();
    acl_manager_->RegisterCallbacks(&mock_connection_callback_, client_handler_);
  }
};

class AclManagerWithConnectionTest : public AclManagerTest {
 protected:
  void SetUp() override {
    AclManagerTest::SetUp();
    test_hci_layer_->RegisterEventHandler(
        EventCode::COMMAND_COMPLETE,
        base::Bind(&TestHciLayer::CommandCompleteCallback, common::Unretained(test_hci_layer_)), nullptr);

    handle_ = 0x123;
    acl_manager_->CreateConnection(remote);

    // Wait for the connection request
    std::unique_ptr<CommandPacketBuilder> last_command;
    do {
      last_command = test_hci_layer_->GetLastCommand();
    } while (last_command == nullptr);

    auto first_connection = GetConnectionFuture();
    test_hci_layer_->IncomingEvent(
        ConnectionCompleteBuilder::Create(ErrorCode::SUCCESS, handle_, remote, LinkType::ACL, Enable::DISABLED));

    auto first_connection_status = first_connection.wait_for(kTimeout);
    ASSERT_EQ(first_connection_status, std::future_status::ready);

    connection_ = GetLastConnection();
    connection_->RegisterCallbacks(&mock_connection_management_callbacks_, client_handler_);
  }

  uint16_t handle_;
  std::shared_ptr<AclConnection> connection_;

  class MockConnectionManagementCallbacks : public ConnectionManagementCallbacks {
   public:
    MOCK_METHOD1(OnConnectionPacketTypeChanged, void(uint16_t packet_type));
    MOCK_METHOD0(OnAuthenticationComplete, void());
    MOCK_METHOD1(OnEncryptionChange, void(EncryptionEnabled enabled));
    MOCK_METHOD0(OnChangeConnectionLinkKeyComplete, void());
    MOCK_METHOD1(OnReadClockOffsetComplete, void(uint16_t clock_offse));
    MOCK_METHOD2(OnModeChange, void(Mode current_mode, uint16_t interval));
    MOCK_METHOD5(OnQosSetupComplete, void(ServiceType service_type, uint32_t token_rate, uint32_t peak_bandwidth,
                                          uint32_t latency, uint32_t delay_variation));
    MOCK_METHOD6(OnFlowSpecificationComplete,
                 void(FlowDirection flow_direction, ServiceType service_type, uint32_t token_rate,
                      uint32_t token_bucket_size, uint32_t peak_bandwidth, uint32_t access_latency));
    MOCK_METHOD0(OnFlushOccurred, void());
    MOCK_METHOD1(OnRoleDiscoveryComplete, void(Role current_role));
    MOCK_METHOD1(OnReadLinkPolicySettingsComplete, void(uint16_t link_policy_settings));
    MOCK_METHOD1(OnReadAutomaticFlushTimeoutComplete, void(uint16_t flush_timeout));
    MOCK_METHOD1(OnReadTransmitPowerLevelComplete, void(uint8_t transmit_power_level));
    MOCK_METHOD1(OnReadLinkSupervisionTimeoutComplete, void(uint16_t link_supervision_timeout));
    MOCK_METHOD1(OnReadFailedContactCounterComplete, void(uint16_t failed_contact_counter));
    MOCK_METHOD1(OnReadLinkQualityComplete, void(uint8_t link_quality));
    MOCK_METHOD1(OnReadRssiComplete, void(uint8_t rssi));
    MOCK_METHOD2(OnReadClockComplete, void(uint32_t clock, uint16_t accuracy));
  } mock_connection_management_callbacks_;
};

TEST_F(AclManagerTest, startup_teardown) {}

TEST_F(AclManagerNoCallbacksTest, acl_connection_before_registered_callbacks) {
  ClassOfDevice class_of_device;

  test_hci_layer_->IncomingEvent(
      ConnectionRequestBuilder::Create(remote, class_of_device, ConnectionRequestLinkType::ACL));
  fake_registry_.SynchronizeModuleHandler(&HciLayer::Factory, std::chrono::milliseconds(20));
  fake_registry_.SynchronizeModuleHandler(&AclManager::Factory, std::chrono::milliseconds(20));
  fake_registry_.SynchronizeModuleHandler(&HciLayer::Factory, std::chrono::milliseconds(20));
  auto last_command = test_hci_layer_->GetLastCommand();
  auto packet = GetPacketView(std::move(last_command));
  CommandPacketView command = CommandPacketView::Create(packet);
  EXPECT_TRUE(command.IsValid());
  OpCode op_code = command.GetOpCode();
  EXPECT_EQ(op_code, OpCode::REJECT_CONNECTION_REQUEST);
}

TEST_F(AclManagerTest, invoke_registered_callback_connection_complete_success) {
  uint16_t handle = 1;

  acl_manager_->CreateConnection(remote);

  // Wait for the connection request
  std::unique_ptr<CommandPacketBuilder> last_command;
  do {
    last_command = test_hci_layer_->GetLastCommand();
  } while (last_command == nullptr);

  auto first_connection = GetConnectionFuture();

  test_hci_layer_->IncomingEvent(
      ConnectionCompleteBuilder::Create(ErrorCode::SUCCESS, handle, remote, LinkType::ACL, Enable::DISABLED));

  auto first_connection_status = first_connection.wait_for(kTimeout);
  ASSERT_EQ(first_connection_status, std::future_status::ready);

  std::shared_ptr<AclConnection> connection = GetLastConnection();
  ASSERT_EQ(connection->GetAddress(), remote);
}

TEST_F(AclManagerTest, invoke_registered_callback_connection_complete_fail) {
  uint16_t handle = 0x123;

  acl_manager_->CreateConnection(remote);

  // Wait for the connection request
  std::unique_ptr<CommandPacketBuilder> last_command;
  do {
    last_command = test_hci_layer_->GetLastCommand();
  } while (last_command == nullptr);

  EXPECT_CALL(mock_connection_callback_, OnConnectFail(remote, ErrorCode::PAGE_TIMEOUT));
  test_hci_layer_->IncomingEvent(
      ConnectionCompleteBuilder::Create(ErrorCode::PAGE_TIMEOUT, handle, remote, LinkType::ACL, Enable::DISABLED));
  fake_registry_.SynchronizeModuleHandler(&HciLayer::Factory, std::chrono::milliseconds(20));
  fake_registry_.SynchronizeModuleHandler(&AclManager::Factory, std::chrono::milliseconds(20));
  fake_registry_.SynchronizeModuleHandler(&HciLayer::Factory, std::chrono::milliseconds(20));
}

TEST_F(AclManagerTest, invoke_registered_callback_disconnection_complete) {
  uint16_t handle = 0x123;

  acl_manager_->CreateConnection(remote);

  // Wait for the connection request
  std::unique_ptr<CommandPacketBuilder> last_command;
  do {
    last_command = test_hci_layer_->GetLastCommand();
  } while (last_command == nullptr);

  auto first_connection = GetConnectionFuture();

  test_hci_layer_->IncomingEvent(
      ConnectionCompleteBuilder::Create(ErrorCode::SUCCESS, handle, remote, LinkType::ACL, Enable::DISABLED));

  auto first_connection_status = first_connection.wait_for(kTimeout);
  ASSERT_EQ(first_connection_status, std::future_status::ready);

  std::shared_ptr<AclConnection> connection = GetLastConnection();

  // Register the disconnect handler
  std::promise<ErrorCode> promise;
  auto future = promise.get_future();
  connection->RegisterDisconnectCallback(
      common::BindOnce([](std::promise<ErrorCode> promise, ErrorCode reason) { promise.set_value(reason); },
                       std::move(promise)),
      client_handler_);

  test_hci_layer_->IncomingEvent(
      DisconnectionCompleteBuilder::Create(ErrorCode::SUCCESS, handle, ErrorCode::REMOTE_USER_TERMINATED_CONNECTION));

  auto disconnection_status = future.wait_for(kTimeout);
  ASSERT_EQ(disconnection_status, std::future_status::ready);
  ASSERT_EQ(ErrorCode::REMOTE_USER_TERMINATED_CONNECTION, future.get());

  fake_registry_.SynchronizeModuleHandler(&HciLayer::Factory, std::chrono::milliseconds(20));
}

TEST_F(AclManagerTest, acl_connection_finish_after_disconnected) {
  uint16_t handle = 0x123;

  acl_manager_->CreateConnection(remote);

  // Wait for the connection request
  std::unique_ptr<CommandPacketBuilder> last_command;
  do {
    last_command = test_hci_layer_->GetLastCommand();
  } while (last_command == nullptr);

  auto first_connection = GetConnectionFuture();

  test_hci_layer_->IncomingEvent(
      ConnectionCompleteBuilder::Create(ErrorCode::SUCCESS, handle, remote, LinkType::ACL, Enable::DISABLED));

  auto first_connection_status = first_connection.wait_for(kTimeout);
  ASSERT_EQ(first_connection_status, std::future_status::ready);

  std::shared_ptr<AclConnection> connection = GetLastConnection();

  // Register the disconnect handler
  std::promise<ErrorCode> promise;
  auto future = promise.get_future();
  connection->RegisterDisconnectCallback(
      common::BindOnce([](std::promise<ErrorCode> promise, ErrorCode reason) { promise.set_value(reason); },
                       std::move(promise)),
      client_handler_);

  test_hci_layer_->IncomingEvent(DisconnectionCompleteBuilder::Create(
      ErrorCode::SUCCESS, handle, ErrorCode::REMOTE_DEVICE_TERMINATED_CONNECTION_POWER_OFF));

  auto disconnection_status = future.wait_for(kTimeout);
  ASSERT_EQ(disconnection_status, std::future_status::ready);
  ASSERT_EQ(ErrorCode::REMOTE_DEVICE_TERMINATED_CONNECTION_POWER_OFF, future.get());

  connection->Finish();
}

TEST_F(AclManagerTest, acl_send_data_one_connection) {
  uint16_t handle = 0x123;

  acl_manager_->CreateConnection(remote);

  // Wait for the connection request
  std::unique_ptr<CommandPacketBuilder> last_command;
  do {
    last_command = test_hci_layer_->GetLastCommand();
  } while (last_command == nullptr);

  auto first_connection = GetConnectionFuture();

  test_hci_layer_->IncomingEvent(
      ConnectionCompleteBuilder::Create(ErrorCode::SUCCESS, handle, remote, LinkType::ACL, Enable::DISABLED));

  auto first_connection_status = first_connection.wait_for(kTimeout);
  ASSERT_EQ(first_connection_status, std::future_status::ready);

  std::shared_ptr<AclConnection> connection = GetLastConnection();

  // Register the disconnect handler
  connection->RegisterDisconnectCallback(
      common::Bind([](std::shared_ptr<AclConnection> conn, ErrorCode) { conn->Finish(); }, connection),
      client_handler_);

  // Send a packet from HCI
  test_hci_layer_->IncomingAclData(handle);
  auto queue_end = connection->GetAclQueueEnd();

  std::unique_ptr<PacketView<kLittleEndian>> received;
  do {
    received = queue_end->TryDequeue();
  } while (received == nullptr);

  PacketView<kLittleEndian> received_packet = *received;

  // Send a packet from the connection
  SendAclData(handle, connection);

  auto sent_packet = test_hci_layer_->OutgoingAclData();

  // Send another packet from the connection
  SendAclData(handle, connection);

  sent_packet = test_hci_layer_->OutgoingAclData();
  connection->Disconnect(DisconnectReason::AUTHENTICATION_FAILURE);
}

TEST_F(AclManagerTest, acl_send_data_credits) {
  uint16_t handle = 0x123;

  acl_manager_->CreateConnection(remote);

  // Wait for the connection request
  std::unique_ptr<CommandPacketBuilder> last_command;
  do {
    last_command = test_hci_layer_->GetLastCommand();
  } while (last_command == nullptr);

  auto first_connection = GetConnectionFuture();
  test_hci_layer_->IncomingEvent(
      ConnectionCompleteBuilder::Create(ErrorCode::SUCCESS, handle, remote, LinkType::ACL, Enable::DISABLED));

  auto first_connection_status = first_connection.wait_for(kTimeout);
  ASSERT_EQ(first_connection_status, std::future_status::ready);

  std::shared_ptr<AclConnection> connection = GetLastConnection();

  // Register the disconnect handler
  connection->RegisterDisconnectCallback(
      common::BindOnce([](std::shared_ptr<AclConnection> conn, ErrorCode) { conn->Finish(); }, connection),
      client_handler_);

  // Use all the credits
  for (uint16_t credits = 0; credits < test_controller_->total_acl_buffers_; credits++) {
    // Send a packet from the connection
    SendAclData(handle, connection);

    auto sent_packet = test_hci_layer_->OutgoingAclData();
  }

  // Send another packet from the connection
  SendAclData(handle, connection);

  test_hci_layer_->AssertNoOutgoingAclData();

  test_controller_->CompletePackets(handle, 1);

  auto after_credits_sent_packet = test_hci_layer_->OutgoingAclData();

  connection->Disconnect(DisconnectReason::AUTHENTICATION_FAILURE);
}

TEST_F(AclManagerWithConnectionTest, send_change_connection_packet_type) {
  connection_->ChangeConnectionPacketType(0xEE1C);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::CHANGE_CONNECTION_PACKET_TYPE);
  auto command_view = ChangeConnectionPacketTypeView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetPacketType(), 0xEE1C);

  EXPECT_CALL(mock_connection_management_callbacks_, OnConnectionPacketTypeChanged(0xEE1C));
  test_hci_layer_->IncomingEvent(ConnectionPacketTypeChangedBuilder::Create(ErrorCode::SUCCESS, handle_, 0xEE1C));
}

TEST_F(AclManagerWithConnectionTest, send_authentication_requested) {
  connection_->AuthenticationRequested();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::AUTHENTICATION_REQUESTED);
  auto command_view = AuthenticationRequestedView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnAuthenticationComplete);
  test_hci_layer_->IncomingEvent(AuthenticationCompleteBuilder::Create(ErrorCode::SUCCESS, handle_));
}

TEST_F(AclManagerWithConnectionTest, send_set_connection_encryption) {
  connection_->SetConnectionEncryption(Enable::ENABLED);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::SET_CONNECTION_ENCRYPTION);
  auto command_view = SetConnectionEncryptionView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetEncryptionEnable(), Enable::ENABLED);

  EXPECT_CALL(mock_connection_management_callbacks_, OnEncryptionChange(EncryptionEnabled::BR_EDR_AES_CCM));
  test_hci_layer_->IncomingEvent(
      EncryptionChangeBuilder::Create(ErrorCode::SUCCESS, handle_, EncryptionEnabled::BR_EDR_AES_CCM));
}

TEST_F(AclManagerWithConnectionTest, send_change_connection_link_key) {
  connection_->ChangeConnectionLinkKey();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::CHANGE_CONNECTION_LINK_KEY);
  auto command_view = ChangeConnectionLinkKeyView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnChangeConnectionLinkKeyComplete);
  test_hci_layer_->IncomingEvent(ChangeConnectionLinkKeyCompleteBuilder::Create(ErrorCode::SUCCESS, handle_));
}

TEST_F(AclManagerWithConnectionTest, send_read_clock_offset) {
  connection_->ReadClockOffset();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::READ_CLOCK_OFFSET);
  auto command_view = ReadClockOffsetView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnReadClockOffsetComplete(0x0123));
  test_hci_layer_->IncomingEvent(ReadClockOffsetCompleteBuilder::Create(ErrorCode::SUCCESS, handle_, 0x0123));
}

TEST_F(AclManagerWithConnectionTest, send_hold_mode) {
  connection_->HoldMode(0x0500, 0x0020);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::HOLD_MODE);
  auto command_view = HoldModeView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetHoldModeMaxInterval(), 0x0500);
  EXPECT_EQ(command_view.GetHoldModeMinInterval(), 0x0020);

  EXPECT_CALL(mock_connection_management_callbacks_, OnModeChange(Mode::HOLD, 0x0020));
  test_hci_layer_->IncomingEvent(ModeChangeBuilder::Create(ErrorCode::SUCCESS, handle_, Mode::HOLD, 0x0020));
}

TEST_F(AclManagerWithConnectionTest, send_sniff_mode) {
  connection_->SniffMode(0x0500, 0x0020, 0x0040, 0x0014);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::SNIFF_MODE);
  auto command_view = SniffModeView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetSniffMaxInterval(), 0x0500);
  EXPECT_EQ(command_view.GetSniffMinInterval(), 0x0020);
  EXPECT_EQ(command_view.GetSniffAttempt(), 0x0040);
  EXPECT_EQ(command_view.GetSniffTimeout(), 0x0014);

  EXPECT_CALL(mock_connection_management_callbacks_, OnModeChange(Mode::SNIFF, 0x0028));
  test_hci_layer_->IncomingEvent(ModeChangeBuilder::Create(ErrorCode::SUCCESS, handle_, Mode::SNIFF, 0x0028));
}

TEST_F(AclManagerWithConnectionTest, send_exit_sniff_mode) {
  connection_->ExitSniffMode();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::EXIT_SNIFF_MODE);
  auto command_view = ExitSniffModeView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnModeChange(Mode::ACTIVE, 0x00));
  test_hci_layer_->IncomingEvent(ModeChangeBuilder::Create(ErrorCode::SUCCESS, handle_, Mode::ACTIVE, 0x00));
}

TEST_F(AclManagerWithConnectionTest, send_qos_setup) {
  connection_->QosSetup(ServiceType::BEST_EFFORT, 0x1234, 0x1233, 0x1232, 0x1231);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::QOS_SETUP);
  auto command_view = QosSetupView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetServiceType(), ServiceType::BEST_EFFORT);
  EXPECT_EQ(command_view.GetTokenRate(), 0x1234);
  EXPECT_EQ(command_view.GetPeakBandwidth(), 0x1233);
  EXPECT_EQ(command_view.GetLatency(), 0x1232);
  EXPECT_EQ(command_view.GetDelayVariation(), 0x1231);

  EXPECT_CALL(mock_connection_management_callbacks_,
              OnQosSetupComplete(ServiceType::BEST_EFFORT, 0x1234, 0x1233, 0x1232, 0x1231));
  test_hci_layer_->IncomingEvent(QosSetupCompleteBuilder::Create(ErrorCode::SUCCESS, handle_, ServiceType::BEST_EFFORT,
                                                                 0x1234, 0x1233, 0x1232, 0x1231));
}

TEST_F(AclManagerWithConnectionTest, send_flow_specification) {
  connection_->FlowSpecification(FlowDirection::OUTGOING_FLOW, ServiceType::BEST_EFFORT, 0x1234, 0x1233, 0x1232,
                                 0x1231);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::FLOW_SPECIFICATION);
  auto command_view = FlowSpecificationView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetFlowDirection(), FlowDirection::OUTGOING_FLOW);
  EXPECT_EQ(command_view.GetServiceType(), ServiceType::BEST_EFFORT);
  EXPECT_EQ(command_view.GetTokenRate(), 0x1234);
  EXPECT_EQ(command_view.GetTokenBucketSize(), 0x1233);
  EXPECT_EQ(command_view.GetPeakBandwidth(), 0x1232);
  EXPECT_EQ(command_view.GetAccessLatency(), 0x1231);

  EXPECT_CALL(mock_connection_management_callbacks_,
              OnFlowSpecificationComplete(FlowDirection::OUTGOING_FLOW, ServiceType::BEST_EFFORT, 0x1234, 0x1233,
                                          0x1232, 0x1231));
  test_hci_layer_->IncomingEvent(
      FlowSpecificationCompleteBuilder::Create(ErrorCode::SUCCESS, handle_, FlowDirection::OUTGOING_FLOW,
                                               ServiceType::BEST_EFFORT, 0x1234, 0x1233, 0x1232, 0x1231));
}

TEST_F(AclManagerWithConnectionTest, send_flush) {
  connection_->Flush();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::FLUSH);
  auto command_view = FlushView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnFlushOccurred());
  test_hci_layer_->IncomingEvent(FlushOccurredBuilder::Create(handle_));
}

TEST_F(AclManagerWithConnectionTest, send_role_discovery) {
  connection_->RoleDiscovery();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::ROLE_DISCOVERY);
  auto command_view = RoleDiscoveryView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnRoleDiscoveryComplete(Role::MASTER));
  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      RoleDiscoveryCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_, Role::MASTER));
}

TEST_F(AclManagerWithConnectionTest, send_read_link_policy_settings) {
  connection_->ReadLinkPolicySettings();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::READ_LINK_POLICY_SETTINGS);
  auto command_view = ReadLinkPolicySettingsView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnReadLinkPolicySettingsComplete(0x07));
  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      ReadLinkPolicySettingsCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_, 0x07));
}

TEST_F(AclManagerWithConnectionTest, send_write_link_policy_settings) {
  connection_->WriteLinkPolicySettings(0x05);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::WRITE_LINK_POLICY_SETTINGS);
  auto command_view = WriteLinkPolicySettingsView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetLinkPolicySettings(), 0x05);

  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      WriteLinkPolicySettingsCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_));
}

TEST_F(AclManagerWithConnectionTest, send_sniff_subrating) {
  connection_->SniffSubrating(0x1234, 0x1235, 0x1236);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::SNIFF_SUBRATING);
  auto command_view = SniffSubratingView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetMaximumLatency(), 0x1234);
  EXPECT_EQ(command_view.GetMinimumRemoteTimeout(), 0x1235);
  EXPECT_EQ(command_view.GetMinimumLocalTimeout(), 0x1236);

  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(SniffSubratingCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_));
}

TEST_F(AclManagerWithConnectionTest, send_read_automatic_flush_timeout) {
  connection_->ReadAutomaticFlushTimeout();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::READ_AUTOMATIC_FLUSH_TIMEOUT);
  auto command_view = ReadAutomaticFlushTimeoutView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnReadAutomaticFlushTimeoutComplete(0x07ff));
  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      ReadAutomaticFlushTimeoutCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_, 0x07ff));
}

TEST_F(AclManagerWithConnectionTest, send_write_automatic_flush_timeout) {
  connection_->WriteAutomaticFlushTimeout(0x07FF);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::WRITE_AUTOMATIC_FLUSH_TIMEOUT);
  auto command_view = WriteAutomaticFlushTimeoutView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetFlushTimeout(), 0x07FF);

  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      WriteAutomaticFlushTimeoutCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_));
}

TEST_F(AclManagerWithConnectionTest, send_read_transmit_power_level) {
  connection_->ReadTransmitPowerLevel(TransmitPowerLevelType::CURRENT);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::READ_TRANSMIT_POWER_LEVEL);
  auto command_view = ReadTransmitPowerLevelView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetType(), TransmitPowerLevelType::CURRENT);

  EXPECT_CALL(mock_connection_management_callbacks_, OnReadTransmitPowerLevelComplete(0x07));
  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      ReadTransmitPowerLevelCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_, 0x07));
}

TEST_F(AclManagerWithConnectionTest, send_read_link_supervision_timeout) {
  connection_->ReadLinkSupervisionTimeout();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::READ_LINK_SUPERVISION_TIMEOUT);
  auto command_view = ReadLinkSupervisionTimeoutView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnReadLinkSupervisionTimeoutComplete(0x5677));
  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      ReadLinkSupervisionTimeoutCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_, 0x5677));
}

TEST_F(AclManagerWithConnectionTest, send_write_link_supervision_timeout) {
  connection_->WriteLinkSupervisionTimeout(0x5678);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::WRITE_LINK_SUPERVISION_TIMEOUT);
  auto command_view = WriteLinkSupervisionTimeoutView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetLinkSupervisionTimeout(), 0x5678);

  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      WriteLinkSupervisionTimeoutCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_));
}

TEST_F(AclManagerWithConnectionTest, send_read_failed_contact_counter) {
  connection_->ReadFailedContactCounter();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::READ_FAILED_CONTACT_COUNTER);
  auto command_view = ReadFailedContactCounterView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnReadFailedContactCounterComplete(0x00));
  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      ReadFailedContactCounterCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_, 0x00));
}

TEST_F(AclManagerWithConnectionTest, send_reset_failed_contact_counter) {
  connection_->ResetFailedContactCounter();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::RESET_FAILED_CONTACT_COUNTER);
  auto command_view = ResetFailedContactCounterView::Create(packet);
  ASSERT(command_view.IsValid());

  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      ResetFailedContactCounterCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_));
}

TEST_F(AclManagerWithConnectionTest, send_read_link_quality) {
  connection_->ReadLinkQuality();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::READ_LINK_QUALITY);
  auto command_view = ReadLinkQualityView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnReadLinkQualityComplete(0xa9));
  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      ReadLinkQualityCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_, 0xa9));
}

TEST_F(AclManagerWithConnectionTest, send_read_rssi) {
  connection_->ReadRssi();
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::READ_RSSI);
  auto command_view = ReadRssiView::Create(packet);
  ASSERT(command_view.IsValid());

  EXPECT_CALL(mock_connection_management_callbacks_, OnReadRssiComplete(0x00));
  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(ReadRssiCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_, 0x00));
}

TEST_F(AclManagerWithConnectionTest, send_read_clock) {
  connection_->ReadClock(WhichClock::LOCAL);
  auto packet = test_hci_layer_->GetCommandPacket(OpCode::READ_CLOCK);
  auto command_view = ReadClockView::Create(packet);
  ASSERT(command_view.IsValid());
  EXPECT_EQ(command_view.GetWhichClock(), WhichClock::LOCAL);

  EXPECT_CALL(mock_connection_management_callbacks_, OnReadClockComplete(0x00002e6a, 0x0000));
  uint8_t num_packets = 1;
  test_hci_layer_->IncomingEvent(
      ReadClockCompleteBuilder::Create(num_packets, ErrorCode::SUCCESS, handle_, 0x00002e6a, 0x0000));
}

}  // namespace
}  // namespace hci
}  // namespace bluetooth
