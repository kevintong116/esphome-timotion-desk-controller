#include "timotion_desk_controller.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include <string>

namespace esphome {
namespace timotion_desk_controller {

static const char *TAG = "timotion_desk_controller";

static const float DESK_MIN_HEIGHT = 65;
static const float DESK_MAX_HEIGHT = 130;

static float transform_height_to_position(float height) {
  return (height - DESK_MIN_HEIGHT) / (DESK_MAX_HEIGHT - DESK_MIN_HEIGHT);
}
static float transform_position_to_height(float position) { return position * DESK_MAX_HEIGHT; }

void TimotionDeskControllerComponent::loop() {}

void TimotionDeskControllerComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Timotion Desk Controller...");
  this->set_interval("update_desk", 200, [this]() { this->move_desk_(); });
}

void TimotionDeskControllerComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Timotion Desk Controller:");
  ESP_LOGCONFIG(TAG, "  MAC address        : %s", this->parent()->address_str().c_str());
  ESP_LOGCONFIG(TAG, "  Notifications      : %s", this->notify_disable_ ? "disable" : "enable");
  LOG_COVER("  ", "Desk", this);
}

void TimotionDeskControllerComponent::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                                        esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Error writing char at handle %d, status=%d", param->write.handle, param->write.status);
      }
      break;
    }

    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "[%s] Connected successfully!", this->get_name().c_str());
        break;
      }
      break;
    }

    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "[%s] Disconnected!", this->get_name().c_str());
      this->status_set_warning();
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      // Look for output handle
      this->output_handle_ = 0;
      auto chr_output = this->parent()->get_characteristic(this->output_service_uuid_, this->output_char_uuid_);
      if (chr_output == nullptr) {
        this->status_set_warning();
        ESP_LOGW(TAG, "No characteristic found at service %s char %s", this->output_service_uuid_.to_string().c_str(),
                 this->output_char_uuid_.to_string().c_str());
        break;
      }
      this->output_handle_ = chr_output->handle;

      // Register for notification
      auto status_notify = esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(),
                                                             this->parent()->get_remote_bda(), this->output_handle_);
      if (status_notify) {
        ESP_LOGW(TAG, "esp_ble_gattc_register_for_notify failed, status=%d", status_notify);
      }

      // Look for input handle
      this->input_handle_ = 0;
      auto chr_input = this->parent()->get_characteristic(this->input_service_uuid_, this->input_char_uuid_);
      if (chr_input == nullptr) {
        this->status_set_warning();
        ESP_LOGW(TAG, "No characteristic found at service %s char %s", this->input_service_uuid_.to_string().c_str(),
                 this->input_char_uuid_.to_string().c_str());
        break;
      }
      this->input_handle_ = chr_input->handle;

      // Look for control handle
      this->control_handle_ = 0;
      auto chr_control = this->parent()->get_characteristic(this->control_service_uuid_, this->control_char_uuid_);
      if (chr_control == nullptr) {
        this->status_set_warning();
        ESP_LOGW(TAG, "No characteristic found at service %s char %s", this->control_service_uuid_.to_string().c_str(),
                 this->control_char_uuid_.to_string().c_str());
        break;
      }
      this->control_handle_ = chr_control->handle;
      ESP_LOGCONFIG(TAG, "control handle: %d", this->control_handle_);
      this->set_timeout("desk_init", 5000, [this]() { this->read_value_(this->output_handle_); });

      break;
    }

    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.conn_id != this->parent()->get_conn_id())
        break;
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Error reading char at handle %d, status=%d", param->read.handle, param->read.status);
        break;
      }
      if (param->read.handle == this->output_handle_) {
        this->status_clear_warning();
        this->publish_cover_state_(param->read.value, param->read.value_len);
      }
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.conn_id != this->parent()->get_conn_id() || param->notify.handle != this->output_handle_)
        break;
      ESP_LOGV(TAG, "[%s] ESP_GATTC_NOTIFY_EVT: handle=0x%x, value=0x%x", this->get_name().c_str(),
               param->notify.handle, param->notify.value[0]);
      this->publish_cover_state_(param->notify.value, param->notify.value_len);
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      this->node_state = espbt::ClientState::ESTABLISHED;
      if (param->reg_for_notify.status == ESP_GATT_OK) {
        this->notify_disable_ = false;
      }
      break;
    }

    default:
      break;
  }
}

void TimotionDeskControllerComponent::write_value_(uint16_t handle, uint64_t value) {
  ESP_LOGD(">>>> ", "write_value_");
  ESP_LOGD(TAG, "handle, value: %d %llu", handle, value);
  uint8_t data[8];
  for (int i = 7; i >= 0; --i) {
    data[7 - i] = (value >> (8 * i)) & 0xFF;
  }

  esp_err_t status = ::esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle,
                                                sizeof(data), data, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);

  if (status != ESP_OK) {
    this->status_set_warning();
    ESP_LOGW(TAG, "[%s] Error sending write request for cover, status=%d", this->get_name().c_str(), status);
  }
}

void TimotionDeskControllerComponent::read_value_(uint16_t handle) {
  auto status_read = esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle,
                                             ESP_GATT_AUTH_REQ_NONE);
  if (status_read) {
    this->status_set_warning();
    ESP_LOGW(TAG, "[%s] Error sending read request for cover, status=%d", this->get_name().c_str(), status_read);
  }
}

cover::CoverTraits TimotionDeskControllerComponent::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_is_assumed_state(false);
  traits.set_supports_position(true);
  traits.set_supports_tilt(false);
  return traits;
}

void TimotionDeskControllerComponent::publish_cover_state_(uint8_t *value, uint16_t value_len) {
  std::vector<uint8_t> x(value, value + value_len);
  
  // Check if desk is moving
  if (x[1] == 1) {
    uint16_t height = (((uint16_t)x[6] << 8) | x[7]) / 10;
    uint16_t speed = x[4];

    if (this->lastHeight == height && this->lastSpeed == speed) return; 
    this->lastHeight = height;
    this->lastSpeed = speed;
  
    float position = transform_height_to_position((float) height);
    ESP_LOGCONFIG(TAG, "publish %d %d %d %d", speed, height, position, this->position);
  
    //   if (speed == 40) {
    if (speed == 101) {
      this->current_operation = cover::COVER_OPERATION_CLOSING;
    } else if (speed == 85) {
      this->current_operation = cover::COVER_OPERATION_OPENING;
    } else if (speed == 5) {
      this->current_operation = cover::COVER_OPERATION_IDLE;
    }
  
    this->position = position;
    this->publish_state(false);
  }
}

void TimotionDeskControllerComponent::move_desk_() {
  if (this->notify_disable_) {
    if (this->controlled_ || this->current_operation != cover::COVER_OPERATION_IDLE) {
      this->read_value_(this->output_handle_);
    }
  }

  if (!this->controlled_) {
    return;
  }

  // Check if target has been reached
  if (this->is_at_target_()) {
    ESP_LOGD(TAG, "Update Desk - target reached");
    this->stop_move_();
    return;
  }

  if (this->notify_disable_) {
    if (this->current_operation == cover::COVER_OPERATION_IDLE) {
      this->not_moving_loop_++;
      if (this->not_moving_loop_ > 4) {
        ESP_LOGD(TAG, "Update Desk - desk not moving");
        this->stop_move_();
      }
    } else {
      this->not_moving_loop_ = 0;
    }
  }

  ESP_LOGD(TAG, "Update Desk - Move from %.0f to %.0f", this->position * 100, this->position_target_ * 100);
  this->move_torwards_();
}

void TimotionDeskControllerComponent::control(const cover::CoverCall &call) {
  if (this->notify_disable_) {
    this->read_value_(this->output_handle_);
  }

  if (call.get_position().has_value()) {
    if (this->current_operation != cover::COVER_OPERATION_IDLE) {
      this->stop_move_();
    }

    this->position_target_ = *call.get_position();

    if (this->position == this->position_target_) {
      return;
    }

    if (this->position_target_ > this->position) {
      this->current_operation = cover::COVER_OPERATION_OPENING;
    } else {
      this->current_operation = cover::COVER_OPERATION_CLOSING;
    }

    this->start_move_torwards_();
    return;
  }

  if (call.get_stop()) {
    ESP_LOGD(TAG, "Cover control - STOP");
    this->stop_move_();
  }
}

void TimotionDeskControllerComponent::start_move_torwards_() {
  this->controlled_ = true;
  if (this->notify_disable_) {
    this->not_moving_loop_ = 0;
  }
  //   if (false == this->use_only_up_down_command_) {
  //     this->write_value_(this->control_handle_, 0xFE);
  //     this->write_value_(this->control_handle_, 0xFF);
  //   }
}

void TimotionDeskControllerComponent::move_torwards_() {
  //   if (this->use_only_up_down_command_) {
  if (this->current_operation == cover::COVER_OPERATION_OPENING) {
    //   this->write_value_(this->control_handle_, 0x47);
    this->write_value_(this->control_handle_, 0xdd00710000000576);
  } else if (this->current_operation == cover::COVER_OPERATION_CLOSING) {
    this->write_value_(this->control_handle_, 0xdd00700000000575);
  }
  //   } else {
  //     this->write_value_(this->input_handle_, transform_position_to_height(this->position_target_));
  //   }
}

void TimotionDeskControllerComponent::stop_move_() {
  this->write_value_(this->control_handle_, 0x0000000000); // not needed?
  //   if (false == this->use_only_up_down_command_) {
  //     this->write_value_(this->input_handle_, 0x8001);
  //   }

  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->controlled_ = false;
}

bool TimotionDeskControllerComponent::is_at_target_() const {
  switch (this->current_operation) {
    case cover::COVER_OPERATION_OPENING:
      return this->position >= this->position_target_;
    case cover::COVER_OPERATION_CLOSING:
      return this->position <= this->position_target_;
    case cover::COVER_OPERATION_IDLE:
      if (this->notify_disable_) {
        return !this->controlled_;
      }
    //   return !this->controlled_;
    default:
      return true;
  }
}

espbt::ESPBTUUID uuid128_from_string(std::string value) {
  esp_bt_uuid_t m_uuid;
  m_uuid.len = ESP_UUID_LEN_128;
  int n = 0;
  for (int i = 0; i < value.length();) {
    if (value.c_str()[i] == '-') {
      i++;
    }
    uint8_t MSB = value.c_str()[i];
    uint8_t LSB = value.c_str()[i + 1];
    if (MSB > '9') {
      MSB -= 7;
    }
    if (LSB > '9') {
      LSB -= 7;
    }
    m_uuid.uuid.uuid128[15 - n++] = ((MSB & 0x0F) << 4) | (LSB & 0x0F);
    i += 2;
  }

  return espbt::ESPBTUUID::from_uuid(m_uuid);
}

}  // namespace timotion_desk_controller
}  // namespace esphome
