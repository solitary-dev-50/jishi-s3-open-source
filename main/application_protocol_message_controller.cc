#include "application_protocol_message_controller.h"

#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <esp_log.h>

#include "assets/lang_config.h"
#include "board.h"
#include "mcp_server.h"

namespace {

constexpr const char* TAG = "AppProtoMsg";
constexpr uint32_t kLocalServoCloudSuppressMs = 5000;
constexpr const char* kSuppressedContentRedacted = "[redacted]";

bool ContainsAnyText(const std::string& text, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (needle != nullptr && *needle != '\0' && text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsInternalToolTraceText(const char* text) {
    if (text == nullptr) {
        return false;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    return text[0] == '%' && text[1] != '\0';
}

std::string TrimAsciiWhitespace(const std::string& input) {
    const auto begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = input.find_last_not_of(" \t\r\n");
    return input.substr(begin, end - begin + 1);
}

enum class LocalServoPanCommand {
    None,
    Left,
    Right,
    Center,
};

struct LocalServoPanMatch {
    LocalServoPanCommand command = LocalServoPanCommand::None;
    bool suppress_cloud = false;
    bool direct_pan = false;
};

enum class HomeworkReadModeOutcome {
    None,
    Entered,
    Cancelled,
};

struct HomeworkReadModeTransitionResult {
    HomeworkReadModeOutcome outcome = HomeworkReadModeOutcome::None;
    std::string prompt_text;
    std::string detect_text;
};

bool DetectHomeworkReadModeStartIntent(const std::string& text) {
    return ContainsAnyText(text,
                           {"我来读题",
                            "我来念题",
                            "我给你读题",
                            "我给你念题",
                            "我读给你听",
                            "我能读给你听吗",
                            "我能念给你听吗",
                            "我能给你读题吗",
                            "我能给你念题吗",
                            "我给你念一下",
                            "我把题念给你",
                            "你听我读题",
                            "你听我念题",
                            "我念给你听",
                            "我可以念给你听吗",
                            "我可以读给你听吗",
                            "我可以给你念题吗",
                            "我可以给你读题吗",
                            "进入读题模式",
                            "开始读题",
                            "帮我整理题目",
                            "帮我整理这道题"});
}

bool DetectHomeworkReadModeCloudHint(const std::string& text) {
    return ContainsAnyText(text,
                           {"没看清",
                            "没看清楚",
                            "当然可以，你念吧",
                            "当然可以，你读吧",
                            "你念吧",
                            "你读吧",
                            "我听着呢",
                            "你读给我听",
                            "你读给我听，我帮你整理",
                            "读给我听",
                            "帮你整理题目"});
}

std::string BuildHomeworkReadModeEntryPrompt() {
    return "好，你直接把整道题连续读给我听。读完后按一下聊天键，我再开始帮你讲。";
}

LocalServoPanMatch DetectLocalServoPanCommand(const std::string& text) {
    if (ContainsAnyText(text, {"测试左转", "测试向左转", "测试向左看"})) {
        return {LocalServoPanCommand::Left, true, true};
    }
    if (ContainsAnyText(text, {"测试右转", "测试向右转", "测试向右看"})) {
        return {LocalServoPanCommand::Right, true, true};
    }
    if (ContainsAnyText(text, {"测试回中", "测试看前面"})) {
        return {LocalServoPanCommand::Center, true, true};
    }
    if (ContainsAnyText(text, {"向左转", "向左看", "往左看", "朝左看", "左转头", "往左转"})) {
        return {LocalServoPanCommand::Left, true, false};
    }
    if (ContainsAnyText(text, {"向右转", "向右看", "往右看", "朝右看", "右转头", "往右转"})) {
        return {LocalServoPanCommand::Right, true, false};
    }
    if (ContainsAnyText(text, {"回中", "回到中间", "回正", "看前面", "看正前方", "恢复前方"})) {
        return {LocalServoPanCommand::Center, true, false};
    }
    return {};
}

float ResolveLocalServoPanAngle(LocalServoPanCommand command) {
    switch (command) {
    case LocalServoPanCommand::Left:
        return 120.0f;
    case LocalServoPanCommand::Right:
        return 60.0f;
    case LocalServoPanCommand::Center:
    default:
        return 90.0f;
    }
}

const char* DescribeLocalServoPanCommand(LocalServoPanCommand command) {
    switch (command) {
    case LocalServoPanCommand::Left:
        return "已下发向左看指令，按当前时长预计完成";
    case LocalServoPanCommand::Right:
        return "已下发向右看指令，按当前时长预计完成";
    case LocalServoPanCommand::Center:
        return "已下发回中指令，按当前时长预计完成";
    case LocalServoPanCommand::None:
    default:
        return "";
    }
}

CameraViewPose ResolveLocalServoViewPose(LocalServoPanCommand command) {
    switch (command) {
    case LocalServoPanCommand::Left:
        return CameraViewPose::Left;
    case LocalServoPanCommand::Right:
        return CameraViewPose::Right;
    case LocalServoPanCommand::Center:
    default:
        return CameraViewPose::Default;
    }
}

ServoPanCalibrationAction DetectServoPanCalibrationAction(const std::string& text) {
    if (ContainsAnyText(text, {"开始水平校准", "开始舵机校准", "进入水平校准", "进入舵机校准"})) {
        return ServoPanCalibrationAction::Start;
    }
    if (ContainsAnyText(text, {"左调一点", "往左调一点", "左移一点"})) {
        return ServoPanCalibrationAction::StepLeft;
    }
    if (ContainsAnyText(text, {"右调一点", "往右调一点", "右移一点"})) {
        return ServoPanCalibrationAction::StepRight;
    }
    if (ContainsAnyText(text, {"保存左极限", "记录左极限"})) {
        return ServoPanCalibrationAction::SaveLeftLimit;
    }
    if (ContainsAnyText(text, {"保存右极限", "记录右极限"})) {
        return ServoPanCalibrationAction::SaveRightLimit;
    }
    if (ContainsAnyText(text, {"计算中点", "计算中心", "转到中点", "转到中心"})) {
        return ServoPanCalibrationAction::ComputeCenter;
    }
    if (ContainsAnyText(text, {"查看校准", "校准状态", "当前校准"})) {
        return ServoPanCalibrationAction::Status;
    }
    if (ContainsAnyText(text, {"退出校准", "结束校准", "关闭校准"})) {
        return ServoPanCalibrationAction::Exit;
    }
    return ServoPanCalibrationAction::None;
}

std::optional<float> DetectServoPanCalibrationDeltaDeg(const std::string& text) {
    auto extract_first_number = [](const std::string& value) -> std::optional<float> {
        std::string number;
        bool seen_digit = false;
        for (char ch : value) {
            if ((ch >= '0' && ch <= '9') || (ch == '.' && !number.empty() && number.find('.') == std::string::npos)) {
                number.push_back(ch);
                if (ch >= '0' && ch <= '9') {
                    seen_digit = true;
                }
            } else if (!number.empty()) {
                break;
            }
        }
        if (!seen_digit) {
            return std::nullopt;
        }
        return std::stof(number);
    };

    const bool has_degree = text.find("度") != std::string::npos || text.find("°") != std::string::npos;
    if (!has_degree) {
        return std::nullopt;
    }

    if (ContainsAnyText(text, {"左转", "左调", "往左调", "往左转"})) {
        auto value = extract_first_number(text);
        if (value.has_value()) {
            return *value;
        }
    }
    if (ContainsAnyText(text, {"右转", "右调", "往右调", "往右转"})) {
        auto value = extract_first_number(text);
        if (value.has_value()) {
            return -*value;
        }
    }
    return std::nullopt;
}

}  // namespace

void ApplicationProtocolMessageController::HandleIncomingJson(const cJSON* root,
                                                              Display* display,
                                                              const Callbacks& callbacks) {
    if (root == nullptr || display == nullptr) {
        return;
    }

    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == nullptr) {
        ESP_LOGW(TAG, "Invalid message: missing type");
        return;
    }

    if (strcmp(type->valuestring, "tts") == 0) {
        HomeworkReadModeState read_mode_state_snapshot = HomeworkReadModeState::Idle;
        {
            std::lock_guard<std::mutex> lock(homework_read_mode_mutex_);
            read_mode_state_snapshot = homework_read_mode_state_;
        }
        auto state = cJSON_GetObjectItem(root, "state");
        if (!cJSON_IsString(state) || state->valuestring == nullptr) {
            return;
        }
        if (strcmp(state->valuestring, "start") == 0) {
            if (read_mode_state_snapshot != HomeworkReadModeState::Idle) {
                ESP_LOGI(TAG, "Suppress remote TTS start during homework read mode");
                return;
            }
            callbacks.schedule([callbacks]() {
                callbacks.set_device_state(kDeviceStateSpeaking);
            });
        } else if (strcmp(state->valuestring, "stop") == 0) {
            if (read_mode_state_snapshot != HomeworkReadModeState::Idle) {
                ESP_LOGI(TAG, "Suppress remote TTS stop during homework read mode");
                return;
            }
            callbacks.schedule([callbacks]() {
                if (callbacks.get_device_state() == kDeviceStateSpeaking) {
                    if (callbacks.get_listening_mode() == kListeningModeManualStop) {
                        callbacks.set_device_state(kDeviceStateIdle);
                    } else {
                        callbacks.set_device_state(kDeviceStateListening);
                    }
                }
            });
        } else if (strcmp(state->valuestring, "sentence_start") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text) && text->valuestring != nullptr) {
                if (read_mode_state_snapshot != HomeworkReadModeState::Idle) {
                    ESP_LOGI(TAG, "Suppress remote TTS sentence during homework read mode");
                    return;
                }
                if (DetectHomeworkReadModeCloudHint(text->valuestring)) {
                    std::lock_guard<std::mutex> lock(homework_read_mode_mutex_);
                    homework_read_mode_hint_ = true;
                }
                if (callbacks.is_local_servo_cloud_suppressed()) {
                    ESP_LOGW(TAG, "Suppress remote TTS sentence: %s", kSuppressedContentRedacted);
                    return;
                }
                if (IsInternalToolTraceText(text->valuestring)) {
                    ESP_LOGI(TAG, "Suppress internal tool trace from visible channel");
                    return;
                }
                ESP_LOGI(TAG, "<< %s", text->valuestring);
                callbacks.schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("assistant", message.c_str());
                });
            }
        }
        return;
    }

    if (strcmp(type->valuestring, "stt") == 0) {
        auto text = cJSON_GetObjectItem(root, "text");
        auto speaker = cJSON_GetObjectItem(root, "speaker");
        if (!cJSON_IsString(text) || text->valuestring == nullptr) {
            return;
        }

        const bool has_speaker =
            cJSON_IsString(speaker) && speaker->valuestring != nullptr && speaker->valuestring[0] != '\0';
        if (has_speaker) {
            ESP_LOGI(TAG, ">> [%s] %s", speaker->valuestring, text->valuestring);
        } else {
            ESP_LOGI(TAG, ">> %s", text->valuestring);
        }

        callbacks.schedule([callbacks,
                            display,
                            this,
                            message = std::string(text->valuestring),
                            speaker_name = has_speaker ? std::string(speaker->valuestring) : std::string()]() {

            HomeworkReadModeTransitionResult read_mode_result;
            {
                std::lock_guard<std::mutex> lock(homework_read_mode_mutex_);
                const bool explicit_read_start = DetectHomeworkReadModeStartIntent(message);
                const bool hinted_read_start =
                    homework_read_mode_state_ == HomeworkReadModeState::Idle && homework_read_mode_hint_;
                if (homework_read_mode_state_ == HomeworkReadModeState::Idle && (explicit_read_start || hinted_read_start)) {
                    homework_read_mode_state_ = HomeworkReadModeState::LongListening;
                    homework_read_mode_hint_ = false;
                    read_mode_result.outcome = HomeworkReadModeOutcome::Entered;
                    read_mode_result.prompt_text = BuildHomeworkReadModeEntryPrompt();
                }
            }

            if (read_mode_result.outcome != HomeworkReadModeOutcome::None) {
                display->SetChatMessage("user", message.c_str());
                if (read_mode_result.outcome == HomeworkReadModeOutcome::Entered) {
                    callbacks.abort_speaking(kAbortReasonNone);
                    if (callbacks.play_sound) {
                        callbacks.play_sound(Lang::Sounds::OGG_POPUP);
                    }
                    if (callbacks.request_listening_mode) {
                        callbacks.request_listening_mode(kListeningModeManualStop);
                    }
                }
                if (!read_mode_result.prompt_text.empty()) {
                    display->SetChatMessage("system", read_mode_result.prompt_text.c_str());
                }
                return;
            }

            auto& board = Board::GetInstance();
            const auto calibration_action = DetectServoPanCalibrationAction(message);
            const auto calibration_delta_deg = DetectServoPanCalibrationDeltaDeg(message);

            if (calibration_action != ServoPanCalibrationAction::None) {
                display->SetChatMessage("user", message.c_str());
                callbacks.arm_local_servo_cloud_suppression(kLocalServoCloudSuppressMs);
                callbacks.abort_speaking(kAbortReasonNone);
                std::string result;
                const bool ok = board.ExecuteServoPanCalibrationAction(calibration_action, &result);
                ESP_LOGW(TAG,
                         "Local servo calibration command: text=%s action=%d result=%d detail=%s",
                         message.c_str(),
                         static_cast<int>(calibration_action),
                         ok ? 1 : 0,
                         result.c_str());
                display->SetChatMessage("system",
                                        ok ? result.c_str()
                                           : (result.empty() ? "水平校准指令执行失败" : result.c_str()));
                return;
            }

            if (calibration_delta_deg.has_value()) {
                display->SetChatMessage("user", message.c_str());
                callbacks.arm_local_servo_cloud_suppression(kLocalServoCloudSuppressMs);
                callbacks.abort_speaking(kAbortReasonNone);
                std::string result;
                const bool ok = board.AdjustServoPanCalibration(*calibration_delta_deg, &result);
                ESP_LOGW(TAG,
                         "Local servo calibration delta: text=%s delta=%.1f result=%d detail=%s",
                         message.c_str(),
                         static_cast<double>(*calibration_delta_deg),
                         ok ? 1 : 0,
                         result.c_str());
                display->SetChatMessage("system",
                                        ok ? result.c_str()
                                           : (result.empty() ? "水平校准角度调整失败" : result.c_str()));
                return;
            }

            const auto local_servo_match = DetectLocalServoPanCommand(message);
            if (local_servo_match.command != LocalServoPanCommand::None) {
                if (board.IsServoPanCalibrationActive()) {
                    display->SetChatMessage("user", message.c_str());
                    display->SetChatMessage("system",
                                            "当前在水平校准模式，请说 左调一点 / 右调一点 / 左转N度 / 右转N度");
                    return;
                }

                const float target_pan = ResolveLocalServoPanAngle(local_servo_match.command);
                display->SetChatMessage("user", message.c_str());
                bool ok = false;
                if (local_servo_match.suppress_cloud) {
                    callbacks.arm_local_servo_cloud_suppression(kLocalServoCloudSuppressMs);
                    callbacks.abort_speaking(kAbortReasonNone);
                }
                if (local_servo_match.direct_pan) {
                    ok = board.SetServoPanForTest(target_pan, 700);
                } else {
                    ok = board.SetCameraViewPose(ResolveLocalServoViewPose(local_servo_match.command));
                }
                ESP_LOGW(TAG,
                         "Local servo test command: text=%s target_pan=%.1f result=%d suppress_cloud=%d direct_pan=%d",
                         message.c_str(),
                         target_pan,
                         ok ? 1 : 0,
                         local_servo_match.suppress_cloud ? 1 : 0,
                         local_servo_match.direct_pan ? 1 : 0);
                display->SetChatMessage("system",
                                        ok ? DescribeLocalServoPanCommand(local_servo_match.command)
                                           : "测试指令执行失败");
                return;
            }
            display->SetChatMessage("user", message.c_str());
        });
        return;
    }

    if (strcmp(type->valuestring, "llm") == 0) {
        HomeworkReadModeState read_mode_state_snapshot = HomeworkReadModeState::Idle;
        {
            std::lock_guard<std::mutex> lock(homework_read_mode_mutex_);
            read_mode_state_snapshot = homework_read_mode_state_;
        }
        if (read_mode_state_snapshot != HomeworkReadModeState::Idle) {
            ESP_LOGI(TAG, "Suppress remote LLM message during homework read mode");
            return;
        }
        if (callbacks.is_local_servo_cloud_suppressed()) {
            ESP_LOGW(TAG, "Suppress remote LLM message while local interception is active");
            return;
        }
        auto emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emotion) && emotion->valuestring != nullptr) {
            callbacks.schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                display->SetEmotion(emotion_str.c_str());
            });
        }
        return;
    }

    if (strcmp(type->valuestring, "mcp") == 0) {
        HomeworkReadModeState read_mode_state_snapshot = HomeworkReadModeState::Idle;
        {
            std::lock_guard<std::mutex> lock(homework_read_mode_mutex_);
            read_mode_state_snapshot = homework_read_mode_state_;
        }
        if (read_mode_state_snapshot != HomeworkReadModeState::Idle) {
            ESP_LOGI(TAG, "Suppress remote MCP message during homework read mode");
            return;
        }
        if (callbacks.is_local_servo_cloud_suppressed()) {
            ESP_LOGW(TAG, "Suppress remote MCP message while local interception is active");
            return;
        }
        auto payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload)) {
            McpServer::GetInstance().ParseMessage(payload);
        }
        return;
    }

    if (strcmp(type->valuestring, "system") == 0) {
        auto command = cJSON_GetObjectItem(root, "command");
        if (cJSON_IsString(command) && command->valuestring != nullptr) {
            ESP_LOGI(TAG, "System command: %s", command->valuestring);
            if (strcmp(command->valuestring, "reboot") == 0) {
                callbacks.schedule([callbacks]() {
                    callbacks.reboot();
                });
            } else {
                ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
            }
        }
        return;
    }

    if (strcmp(type->valuestring, "alert") == 0) {
        auto status = cJSON_GetObjectItem(root, "status");
        auto message = cJSON_GetObjectItem(root, "message");
        auto emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
            callbacks.alert(status->valuestring,
                            message->valuestring,
                            emotion->valuestring,
                            Lang::Sounds::OGG_VIBRATION);
        } else {
            ESP_LOGW(TAG, "Alert command requires status, message and emotion");
        }
        return;
    }

#if CONFIG_RECEIVE_CUSTOM_MESSAGE
    if (strcmp(type->valuestring, "custom") == 0) {
        auto payload = cJSON_GetObjectItem(root, "payload");
        ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
        if (cJSON_IsObject(payload)) {
            callbacks.schedule([display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                display->SetChatMessage("system", payload_str.c_str());
            });
        } else {
            ESP_LOGW(TAG, "Invalid custom message format: missing payload");
        }
        return;
    }
#endif

    ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
}

void ApplicationProtocolMessageController::NotifyStopListening(Display* display) {
    bool was_active = false;
    {
        std::lock_guard<std::mutex> lock(homework_read_mode_mutex_);
        was_active = homework_read_mode_state_ != HomeworkReadModeState::Idle;
        homework_read_mode_state_ = HomeworkReadModeState::Idle;
        homework_read_mode_hint_ = false;
    }
    if (was_active && display != nullptr) {
        display->SetChatMessage("system", "好的，我开始整理你刚才读的题目。");
    }
}

