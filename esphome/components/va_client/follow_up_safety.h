#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

namespace esphome {
namespace va_client {

static constexpr uint32_t kProtocolTokenMax = 0x7FFFFFFF;
static constexpr uint32_t kRequestFollowUpMs = 10000;
static constexpr uint32_t kRequestFollowUpCommitTimeoutMs = 5000;
static constexpr uint32_t kRequestFollowUpReadyTimeoutMs = 8000;
static constexpr uint32_t kAbsoluteSessionMaxMs = 120000;
static constexpr uint32_t kMicSendBarrierTimeoutMs = 50;
static constexpr uint32_t kFollowupOpenDelayMaxMs = 5000;
static constexpr size_t kWsTextMessageMaxBytes = 2048;
static constexpr size_t kWsBinaryMessageMaxBytes = 64 * 1024;

// Serializes generation validation with every resulting timer/mic side effect.
// It is recursive because effect helpers compose while retaining one ownership
// boundary. Host concurrency tests use the same lock implementation as firmware.
class GenerationEffectGate {
 public:
  void lock() { this->mutex_.lock(); }
  void unlock() { this->mutex_.unlock(); }

 private:
  std::recursive_mutex mutex_;
};

class TransportAdmissionGate {
 public:
  void on_transport_connected() {
    this->session_admitted_.store(false, std::memory_order_release);
    const bool may_admit =
        !this->close_control_fault_.load(std::memory_order_acquire) ||
        this->disconnect_after_fault_.load(std::memory_order_acquire);
    this->transport_may_admit_.store(may_admit, std::memory_order_release);
  }

  void on_transport_disconnected() {
    this->session_admitted_.store(false, std::memory_order_release);
    this->transport_may_admit_.store(false, std::memory_order_release);
    if (this->close_control_fault_.load(std::memory_order_acquire))
      this->disconnect_after_fault_.store(true, std::memory_order_release);
  }

  bool can_admit_session() const {
    return this->transport_may_admit_.load(std::memory_order_acquire);
  }

  bool on_session_admitted() {
    if (!this->can_admit_session())
      return false;
    this->close_control_fault_.store(false, std::memory_order_release);
    this->disconnect_after_fault_.store(false, std::memory_order_release);
    this->session_admitted_.store(true, std::memory_order_release);
    return true;
  }

  bool on_authoritative_close_failure() {
    this->session_admitted_.store(false, std::memory_order_release);
    const bool first_fault =
        !this->close_control_fault_.exchange(true, std::memory_order_acq_rel);
    if (first_fault)
      this->disconnect_after_fault_.store(false, std::memory_order_release);
    this->transport_may_admit_.store(false, std::memory_order_release);
    return first_fault;
  }

  bool can_start_wake() const {
    return this->session_admitted_.load(std::memory_order_acquire) &&
           !this->close_control_fault_.load(std::memory_order_acquire);
  }

  bool close_control_faulted() const {
    return this->close_control_fault_.load(std::memory_order_acquire);
  }

 private:
  std::atomic_bool session_admitted_{false};
  std::atomic_bool close_control_fault_{false};
  std::atomic_bool disconnect_after_fault_{false};
  std::atomic_bool transport_may_admit_{false};
};

enum class WsMessageType : uint8_t { NONE = 0, TEXT, BINARY };
enum class WsReassemblyStatus : uint8_t {
  NEED_MORE = 0,
  COMPLETE,
  CONTROL,
  REJECTED,
};

struct WsReassemblyResult {
  WsReassemblyStatus status{WsReassemblyStatus::NEED_MORE};
  WsMessageType type{WsMessageType::NONE};
};

// ESP-IDF emits one event per transport-buffer chunk, not one event per
// WebSocket message. This accumulator validates both levels: offsets within a
// frame and continuation-frame sequencing across a fragmented message.
class WsMessageReassembler {
 public:
  WsReassemblyResult push(uint8_t opcode, bool fin, size_t payload_len,
                          size_t payload_offset, const uint8_t *data,
                          size_t data_len) {
    if ((data == nullptr && data_len != 0) || payload_offset > payload_len ||
        data_len > payload_len - payload_offset)
      return this->reject_();

    if (payload_offset == 0) {
      if (this->frame_active_)
        return this->reject_();
      this->frame_active_ = true;
      this->frame_opcode_ = opcode;
      this->frame_fin_ = fin;
      this->frame_payload_len_ = payload_len;
      this->frame_received_ = 0;
      this->control_frame_ = opcode >= 0x08;

      if (this->control_frame_) {
        if ((opcode != 0x08 && opcode != 0x09 && opcode != 0x0A) || !fin ||
            payload_len > 125 || (opcode == 0x08 && payload_len == 1))
          return this->reject_();
      } else if (opcode == 0x01 || opcode == 0x02) {
        if (this->message_active_)
          return this->reject_();
        this->message_.clear();
        this->message_type_ =
            opcode == 0x01 ? WsMessageType::TEXT : WsMessageType::BINARY;
        this->message_active_ = true;
      } else if (opcode == 0x00) {
        if (!this->message_active_)
          return this->reject_();
      } else {
        return this->reject_();
      }

      if (!this->control_frame_) {
        const size_t max_bytes = this->message_type_ == WsMessageType::TEXT
                                     ? kWsTextMessageMaxBytes
                                     : kWsBinaryMessageMaxBytes;
        if (payload_len > max_bytes - this->message_.size())
          return this->reject_();
      }
    } else if (!this->frame_active_ || opcode != this->frame_opcode_ ||
               fin != this->frame_fin_ || payload_len != this->frame_payload_len_ ||
               payload_offset != this->frame_received_) {
      return this->reject_();
    }

    if (!this->control_frame_ && data_len != 0)
      this->message_.insert(this->message_.end(), data, data + data_len);
    this->frame_received_ += data_len;
    if (this->frame_received_ != this->frame_payload_len_)
      return {WsReassemblyStatus::NEED_MORE, WsMessageType::NONE};

    this->frame_active_ = false;
    if (this->control_frame_) {
      this->control_frame_ = false;
      return {WsReassemblyStatus::CONTROL, WsMessageType::NONE};
    }
    if (!fin)
      return {WsReassemblyStatus::NEED_MORE, WsMessageType::NONE};

    const WsMessageType completed_type = this->message_type_;
    const bool invalid_binary =
        completed_type == WsMessageType::BINARY &&
        (this->message_.size() < 2 || (this->message_.size() % 2) != 0);
    if (this->message_.empty() || invalid_binary)
      return this->reject_();
    this->message_active_ = false;
    this->message_type_ = WsMessageType::NONE;
    return {WsReassemblyStatus::COMPLETE, completed_type};
  }

  const std::vector<uint8_t> &message() const { return this->message_; }

  void reset() {
    this->message_.clear();
    this->message_type_ = WsMessageType::NONE;
    this->message_active_ = false;
    this->frame_active_ = false;
    this->control_frame_ = false;
    this->frame_opcode_ = 0;
    this->frame_fin_ = false;
    this->frame_payload_len_ = 0;
    this->frame_received_ = 0;
  }

 private:
  WsReassemblyResult reject_() {
    this->reset();
    return {WsReassemblyStatus::REJECTED, WsMessageType::NONE};
  }

  std::vector<uint8_t> message_;
  WsMessageType message_type_{WsMessageType::NONE};
  bool message_active_{false};
  bool frame_active_{false};
  bool control_frame_{false};
  uint8_t frame_opcode_{0};
  bool frame_fin_{false};
  size_t frame_payload_len_{0};
  size_t frame_received_{0};
};

// The backend protocol is deliberately a small, flat JSON object. Keeping the
// parser here makes malformed and duplicate security fields unambiguous without
// adding a general-purpose JSON dependency to the audio hot path.
class FlatJsonObject {
 public:
  bool parse(const std::string &input) {
    this->field_count_ = 0;
    if (input.empty() || input.size() > kMaxMessageBytes)
      return false;

    size_t pos = 0;
    skip_whitespace_(input, pos);
    if (!consume_(input, pos, '{'))
      return false;
    skip_whitespace_(input, pos);
    if (consume_(input, pos, '}')) {
      skip_whitespace_(input, pos);
      return pos == input.size();
    }

    while (pos < input.size()) {
      if (this->field_count_ >= this->fields_.size())
        return false;

      Field field;
      if (!parse_string_(input, pos, field.key) || this->has(field.key.c_str()))
        return false;
      skip_whitespace_(input, pos);
      if (!consume_(input, pos, ':'))
        return false;
      skip_whitespace_(input, pos);

      if (pos < input.size() && input[pos] == '"') {
        field.type = ValueType::STRING;
        if (!parse_string_(input, pos, field.string_value))
          return false;
      } else if (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') {
        field.type = ValueType::UINT;
        if (!parse_uint_(input, pos, field.uint_value))
          return false;
      } else if (consume_literal_(input, pos, "true")) {
        field.type = ValueType::BOOL;
        field.bool_value = true;
      } else if (consume_literal_(input, pos, "false")) {
        field.type = ValueType::BOOL;
        field.bool_value = false;
      } else if (consume_literal_(input, pos, "null")) {
        field.type = ValueType::NIL;
      } else {
        return false;
      }

      this->fields_[this->field_count_++] = field;
      skip_whitespace_(input, pos);
      if (consume_(input, pos, '}')) {
        skip_whitespace_(input, pos);
        return pos == input.size();
      }
      if (!consume_(input, pos, ','))
        return false;
      skip_whitespace_(input, pos);
    }
    return false;
  }

  bool has(const char *key) const { return this->find_(key) != nullptr; }

  bool get_string(const char *key, std::string &out) const {
    const Field *field = this->find_(key);
    if (field == nullptr || field->type != ValueType::STRING)
      return false;
    out = field->string_value;
    return true;
  }

  bool get_uint(const char *key, uint32_t &out) const {
    const Field *field = this->find_(key);
    if (field == nullptr || field->type != ValueType::UINT)
      return false;
    out = field->uint_value;
    return true;
  }

  bool has_only(std::initializer_list<const char *> allowed) const {
    for (size_t i = 0; i < this->field_count_; i++) {
      bool found = false;
      for (const char *key : allowed) {
        if (this->fields_[i].key == key) {
          found = true;
          break;
        }
      }
      if (!found)
        return false;
    }
    return true;
  }

  bool has_exact(std::initializer_list<const char *> required) const {
    if (this->field_count_ != required.size())
      return false;
    for (const char *key : required) {
      if (!this->has(key))
        return false;
    }
    return true;
  }

 private:
  enum class ValueType : uint8_t { STRING, UINT, BOOL, NIL };

  struct Field {
    std::string key;
    ValueType type{ValueType::NIL};
    std::string string_value;
    uint32_t uint_value{0};
    bool bool_value{false};
  };

  static constexpr size_t kMaxFields = 16;
  static constexpr size_t kMaxMessageBytes = kWsTextMessageMaxBytes;
  std::array<Field, kMaxFields> fields_{};
  size_t field_count_{0};

  const Field *find_(const char *key) const {
    for (size_t i = 0; i < this->field_count_; i++) {
      if (this->fields_[i].key == key)
        return &this->fields_[i];
    }
    return nullptr;
  }

  static void skip_whitespace_(const std::string &input, size_t &pos) {
    while (pos < input.size() &&
           (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\r' || input[pos] == '\n')) {
      pos++;
    }
  }

  static bool consume_(const std::string &input, size_t &pos, char expected) {
    if (pos >= input.size() || input[pos] != expected)
      return false;
    pos++;
    return true;
  }

  static bool consume_literal_(const std::string &input, size_t &pos, const char *literal) {
    size_t cursor = pos;
    for (size_t i = 0; literal[i] != '\0'; i++) {
      if (cursor >= input.size() || input[cursor] != literal[i])
        return false;
      cursor++;
    }
    pos = cursor;
    return true;
  }

  static bool parse_uint_(const std::string &input, size_t &pos, uint32_t &out) {
    if (pos >= input.size() || input[pos] < '0' || input[pos] > '9')
      return false;
    if (input[pos] == '0' && pos + 1 < input.size() && input[pos + 1] >= '0' && input[pos + 1] <= '9')
      return false;

    uint32_t value = 0;
    while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') {
      const uint32_t digit = static_cast<uint32_t>(input[pos] - '0');
      if (value > (UINT32_MAX - digit) / 10u)
        return false;
      value = value * 10u + digit;
      pos++;
    }
    out = value;
    return true;
  }

  static bool is_hex_(char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
  }

  static bool parse_string_(const std::string &input, size_t &pos, std::string &out) {
    if (!consume_(input, pos, '"'))
      return false;
    out.clear();
    while (pos < input.size()) {
      const char value = input[pos++];
      if (value == '"')
        return true;
      if (static_cast<unsigned char>(value) < 0x20)
        return false;
      if (value != '\\') {
        out.push_back(value);
        continue;
      }
      if (pos >= input.size())
        return false;
      const char escaped = input[pos++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escaped);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u':
          if (pos + 4 > input.size())
            return false;
          for (size_t i = 0; i < 4; i++) {
            if (!is_hex_(input[pos + i]))
              return false;
          }
          pos += 4;
          out.push_back('?');
          break;
        default:
          return false;
      }
    }
    return false;
  }
};

struct FollowUpAdmissionContext {
  uint32_t token{0};
  uint32_t request_session_nonce{0};
  uint32_t active_session_nonce{0};
  bool message_shape_valid{false};
  bool token_replayed_or_history_full{true};
  bool physical_wake_active{false};
  bool one_shot_consumed{true};
  bool replying{false};
  bool closed_single_turn{false};
  bool connected{false};
  bool microphone_muted{true};
  bool microphone_streaming{true};
  bool enrollment_active{true};
  bool competing_control_active{true};
  bool active_request{true};
  bool callback_in_flight{true};
};

inline bool should_accept_follow_up(const FollowUpAdmissionContext &context) {
  return context.message_shape_valid && context.token != 0 && context.token <= kProtocolTokenMax &&
         context.request_session_nonce != 0 && context.request_session_nonce <= kProtocolTokenMax &&
         context.token != context.request_session_nonce &&
         context.request_session_nonce == context.active_session_nonce &&
         !context.token_replayed_or_history_full && context.physical_wake_active &&
         !context.one_shot_consumed && context.replying && context.closed_single_turn && context.connected &&
         !context.microphone_muted && !context.microphone_streaming && !context.enrollment_active &&
         !context.competing_control_active && !context.active_request && !context.callback_in_flight;
}

}  // namespace va_client
}  // namespace esphome
