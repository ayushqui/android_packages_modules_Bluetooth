/*
 * Copyright 2024 The Android Open Source Project
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
#include <base/functional/bind.h>

#include "bta/include/bta_gatt_api.h"
#include "bta/include/bta_ras_api.h"
#include "bta/ras/ras_types.h"
#include "os/logging/log_adapter.h"
#include "stack/include/bt_types.h"
#include "stack/include/btm_ble_addr.h"
#include "stack/include/gap_api.h"

using namespace bluetooth;
using namespace ::ras;
using namespace ::ras::feature;
using namespace ::ras::uuid;

namespace {

class RasClientImpl;
RasClientImpl* instance;

class RasClientImpl : public bluetooth::ras::RasClient {
 public:
  struct RasTracker {
    RasTracker(const RawAddress& address) : address_(address) {}
    uint16_t conn_id_;
    RawAddress address_;
    const gatt::Service* service_;
    uint32_t remote_supported_features_;

    const gatt::Characteristic* FindCharacteristicByUuid(Uuid uuid) {
      for (auto& characteristic : service_->characteristics) {
        if (characteristic.uuid == uuid) {
          return &characteristic;
        }
      }
      return nullptr;
    }
    const gatt::Characteristic* FindCharacteristicByHandle(uint16_t handle) {
      for (auto& characteristic : service_->characteristics) {
        if (characteristic.value_handle == handle) {
          return &characteristic;
        }
      }
      return nullptr;
    }
  };

  void Initialize() override {
    BTA_GATTC_AppRegister(
        [](tBTA_GATTC_EVT event, tBTA_GATTC* p_data) {
          if (instance && p_data) instance->GattcCallback(event, p_data);
        },
        base::Bind([](uint8_t client_id, uint8_t status) {
          if (status != GATT_SUCCESS) {
            log::error("Can't start Gatt client for Ranging Service");
            return;
          }
          log::info("Initialize, client_id {}", client_id);
          instance->gatt_if_ = client_id;
        }),
        true);
  }

  void Connect(const RawAddress& address) override {
    log::info("{}", ADDRESS_TO_LOGGABLE_CSTR(address));
    tBLE_BD_ADDR ble_bd_addr;
    ResolveAddress(ble_bd_addr, address);
    log::info("resolve {}", ADDRESS_TO_LOGGABLE_CSTR(ble_bd_addr.bda));

    auto tracker = FindTrackerByAddress(ble_bd_addr.bda);
    if (tracker == nullptr) {
      trackers_.emplace_back(std::make_shared<RasTracker>(ble_bd_addr.bda));
    }
    BTA_GATTC_Open(gatt_if_, ble_bd_addr.bda, BTM_BLE_DIRECT_CONNECTION, false);
  }

  void GattcCallback(tBTA_GATTC_EVT event, tBTA_GATTC* p_data) {
    log::info("event: {}", gatt_client_event_text(event));
    switch (event) {
      case BTA_GATTC_OPEN_EVT: {
        OnGattConnected(p_data->open);
      } break;
      case BTA_GATTC_SEARCH_CMPL_EVT: {
        OnGattServiceSearchComplete(p_data->search_cmpl);
      } break;
      default:
        log::warn("Unhandled event: {}", gatt_client_event_text(event).c_str());
    }
  }

  void OnGattConnected(const tBTA_GATTC_OPEN& evt) {
    log::info("{}, conn_id=0x{:04x}, transport:{}, status:{}",
              ADDRESS_TO_LOGGABLE_CSTR(evt.remote_bda), evt.conn_id,
              bt_transport_text(evt.transport).c_str(),
              gatt_status_text(evt.status).c_str());

    if (evt.transport != BT_TRANSPORT_LE) {
      log::warn("Only LE connection is allowed (transport {})",
                bt_transport_text(evt.transport).c_str());
      BTA_GATTC_Close(evt.conn_id);
      return;
    }

    auto tracker = FindTrackerByAddress(evt.remote_bda);
    if (tracker == nullptr) {
      log::warn("Skipping unknown device, address: {}",
                ADDRESS_TO_LOGGABLE_CSTR(evt.remote_bda));
      BTA_GATTC_Close(evt.conn_id);
      return;
    }

    if (evt.status != GATT_SUCCESS) {
      log::error("Failed to connect to server device {}",
                 ADDRESS_TO_LOGGABLE_CSTR(evt.remote_bda));
      return;
    }
    tracker->conn_id_ = evt.conn_id;
    log::info("Search service");
    BTA_GATTC_ServiceSearchRequest(tracker->conn_id_, &kRangingService);
  }

  void OnGattServiceSearchComplete(const tBTA_GATTC_SEARCH_CMPL& evt) {
    auto tracker = FindTrackerByHandle(evt.conn_id);
    if (tracker == nullptr) {
      log::warn("Can't find tracker for conn_id:{}", evt.conn_id);
      return;
    }

    // Get Ranging Service
    bool service_found = false;
    const std::list<gatt::Service>* all_services =
        BTA_GATTC_GetServices(evt.conn_id);
    for (const auto& service : *all_services) {
      if (service.uuid == kRangingService) {
        tracker->service_ = &service;
        service_found = true;
        break;
      }
    }

    if (!service_found) {
      log::error("Can't find Ranging Service in the services list");
      return;
    } else {
      log::info("Found Ranging Service");
      ListCharacteristic(tracker->service_);
    }

    // Read Ras Features
    log::info("Read Ras Features");
    auto characteristic =
        tracker->FindCharacteristicByUuid(kRasFeaturesCharacteristic);
    if (characteristic == nullptr) {
      log::error("Can not find Characteristic for Ras Features");
      return;
    }
    BTA_GATTC_ReadCharacteristic(
        tracker->conn_id_, characteristic->value_handle, GATT_AUTH_REQ_MITM,
        [](uint16_t conn_id, tGATT_STATUS status, uint16_t handle, uint16_t len,
           uint8_t* value, void* data) {
          instance->OnReadCharacteristicCallback(conn_id, status, handle, len,
                                                 value, data);
        },
        nullptr);

    // Subscribe Characteristics
    SubscribeCharacteristic(tracker, kRasOnDemandDataCharacteristic);
    SubscribeCharacteristic(tracker, kRasControlPointCharacteristic);
    SubscribeCharacteristic(tracker, kRasRangingDataReadyCharacteristic);
    SubscribeCharacteristic(tracker, kRasRangingDataOverWrittenCharacteristic);
  }

  void SubscribeCharacteristic(std::shared_ptr<RasTracker> tracker,
                               const Uuid uuid) {
    auto characteristic = tracker->FindCharacteristicByUuid(uuid);
    if (characteristic == nullptr) {
      log::warn("Can't find characteristic 0x{:04x}", uuid.As16Bit());
      return;
    }
    uint16_t ccc_handle = FindCccHandle(characteristic);
    if (ccc_handle == GAP_INVALID_HANDLE) {
      log::warn("Can't find Client Characteristic Configuration descriptor");
      return;
    }

    tGATT_STATUS register_status = BTA_GATTC_RegisterForNotifications(
        gatt_if_, tracker->address_, characteristic->value_handle);
    if (register_status != GATT_SUCCESS) {
      log::error("Fail to register, {}",
                 gatt_status_text(register_status).c_str());
      return;
    }

    std::vector<uint8_t> value(2);
    uint8_t* value_ptr = value.data();
    UINT16_TO_STREAM(value_ptr, GATT_CHAR_CLIENT_CONFIG_INDICTION);
    BTA_GATTC_WriteCharDescr(
        tracker->conn_id_, ccc_handle, value, GATT_AUTH_REQ_NONE,
        [](uint16_t conn_id, tGATT_STATUS status, uint16_t handle, uint16_t len,
           const uint8_t* value, void* data) {
          if (instance)
            instance->OnDescriptorWrite(conn_id, status, handle, len, value,
                                        data);
        },
        nullptr);
  }

  void OnDescriptorWrite(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                         uint16_t len, const uint8_t* value, void* data) {
    log::info("conn_id:{}, handle:{}, status:{}", conn_id, handle,
              gatt_status_text(status).c_str());
  }

  void ListCharacteristic(const gatt::Service* service) {
    for (auto& characteristic : service->characteristics) {
      log::info("Characteristic uuid: 0x{:04x}, handle:{}, {}",
                characteristic.uuid.As16Bit(), characteristic.value_handle,
                getUuidName(characteristic.uuid).c_str());
      for (auto& descriptor : characteristic.descriptors) {
        log::info("\tDescriptor uuid: 0x{:04x}, handle:{}, {}",
                  descriptor.uuid.As16Bit(), descriptor.handle,
                  getUuidName(descriptor.uuid).c_str());
      }
    }
  }

  void ResolveAddress(tBLE_BD_ADDR& ble_bd_addr, const RawAddress& address) {
    ble_bd_addr.bda = address;
    ble_bd_addr.type = BLE_ADDR_RANDOM;
    maybe_resolve_address(&ble_bd_addr.bda, &ble_bd_addr.type);
  }

  void OnReadCharacteristicCallback(uint16_t conn_id, tGATT_STATUS status,
                                    uint16_t handle, uint16_t len,
                                    uint8_t* value, void* data) {
    log::info("conn_id: {}, handle: {}, len: {}", conn_id, handle, len);
    if (status != GATT_SUCCESS) {
      log::error("Fail with status {}", gatt_status_text(status).c_str());
      return;
    }
    auto tracker = FindTrackerByHandle(conn_id);
    if (tracker == nullptr) {
      log::warn("Can't find tracker for conn_id:{}", conn_id);
      return;
    }
    auto characteristic = tracker->FindCharacteristicByHandle(handle);
    if (characteristic == nullptr) {
      log::warn("Can't find characteristic for handle:{}", handle);
      return;
    }

    uint16_t uuid_16bit = characteristic->uuid.As16Bit();
    log::info("Handle uuid 0x{:04x}, {}", uuid_16bit,
              getUuidName(characteristic->uuid).c_str());

    switch (uuid_16bit) {
      case kRasFeaturesCharacteristic16bit: {
        if (len != kFeatureSize) {
          log::error("Invalid len for Ras features");
          return;
        }
        STREAM_TO_UINT32(tracker->remote_supported_features_, value);
        log::info(
            "Remote supported features : {}",
            getFeaturesString(tracker->remote_supported_features_).c_str());
      } break;
      default:
        log::warn("Unexpected UUID");
    }
  }

  std::string getFeaturesString(uint32_t value) {
    std::stringstream ss;
    ss << value;
    if (value == 0) {
      ss << "|No feature supported";
    } else {
      if ((value & kRealTimeRangingData) != 0) {
        ss << "|Real-time Ranging Data";
      }
      if ((value & kRetrieveLostRangingDataSegments) != 0) {
        ss << "|Retrieve Lost Ranging Data Segments";
      }
      if ((value & kAbortOperation) != 0) {
        ss << "|Abort Operation";
      }
      if ((value & kFilterRangingData) != 0) {
        ss << "|Filter Ranging Data";
      }
      if ((value & kPctPhaseFormat) != 0) {
        ss << "|PCT Phase Format";
      }
    }
    return ss.str();
  }

  uint16_t FindCccHandle(const gatt::Characteristic* characteristic) {
    for (auto descriptor : characteristic->descriptors) {
      if (descriptor.uuid == kClientCharacteristicConfiguration) {
        return descriptor.handle;
      }
    }
    return GAP_INVALID_HANDLE;
  }

  std::shared_ptr<RasTracker> FindTrackerByHandle(uint16_t conn_id) const {
    for (auto tracker : trackers_) {
      if (tracker->conn_id_ == conn_id) {
        return tracker;
      }
    }
    return nullptr;
  }

  std::shared_ptr<RasTracker> FindTrackerByAddress(
      const RawAddress& address) const {
    for (auto tracker : trackers_) {
      if (tracker->address_ == address) {
        return tracker;
      }
    }
    return nullptr;
  }

 private:
  uint16_t gatt_if_;
  std::list<std::shared_ptr<RasTracker>> trackers_;
};

}  // namespace

bluetooth::ras::RasClient* bluetooth::ras::GetRasClient() {
  if (instance == nullptr) {
    instance = new RasClientImpl();
  }
  return instance;
};