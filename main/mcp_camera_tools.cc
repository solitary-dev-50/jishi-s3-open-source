#include "mcp_camera_tools.h"

#include <esp_heap_caps.h>
#include <stdexcept>

#include "board.h"
#include "mcp_server.h"

namespace {
std::string BuildTakePhotoResultText(const std::string& raw_result) {
    if (raw_result.empty()) {
        return "已经拍到照片，但视觉结果为空，请再试一次。";
    }
    cJSON* parsed = cJSON_Parse(raw_result.c_str());
    if (cJSON_IsObject(parsed)) {
        cJSON* text = cJSON_GetObjectItem(parsed, "text");
        if (cJSON_IsString(text) && text->valuestring != nullptr) {
            std::string result = text->valuestring;
            cJSON_Delete(parsed);
            return result;
        }
    }
    if (parsed != nullptr) {
        cJSON_Delete(parsed);
    }
    return raw_result;
}
}

void McpCameraToolsRegistrar::AddCommonTools(McpServer& server) {
#ifdef HAVE_LVGL
    auto& board = Board::GetInstance();
    auto camera = board.GetCamera();
    if (!(camera || board.HasCameraCapability())) {
        return;
    }

    server.AddTool("self.camera.take_photo",
        "Take a photo and explain what is in the image. Args: question.",
        PropertyList({ Property("question", kPropertyTypeString) }),
        [&server, &board](const PropertyList& properties) -> ReturnValue {
            auto question = properties["question"].value<std::string>();
            auto camera = board.GetCamera();
            if (camera != nullptr) {
                if (!server.vision_url_.empty()) {
                    camera->SetExplainUrl(server.vision_url_, server.vision_token_);
                }
                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                return BuildTakePhotoResultText(camera->Explain(question));
            }

            uint8_t* jpeg_buf = nullptr;
            size_t jpeg_len = 0;
            if (!board.CaptureOnce(&jpeg_buf, &jpeg_len) || jpeg_buf == nullptr || jpeg_len == 0) {
                throw std::runtime_error("Failed to capture photo");
            }
            std::unique_ptr<uint8_t, void(*)(void*)> jpeg_guard(jpeg_buf, heap_caps_free);
            return BuildTakePhotoResultText(server.ExplainCapturedPhoto(jpeg_buf, jpeg_len, question));
        });
#endif
}

void McpCameraToolsRegistrar::AddUserOnlyTools(McpServer& server) {
#ifdef HAVE_LVGL
    auto& board = Board::GetInstance();
    auto camera = board.GetCamera();
    if (camera || board.HasCameraCapability()) {
        server.AddUserOnlyTool("self.camera.get_last_capture_debug",
            "Get debug metadata for the most recent camera capture.",
            PropertyList(),
            [&board](const PropertyList&) -> ReturnValue {
                return board.GetLastCaptureDebugJson();
            });

        server.AddUserOnlyTool("self.camera.get_last_capture_image",
            "Return the most recent camera capture as an image.",
            PropertyList(),
            [&board](const PropertyList&) -> ReturnValue {
                std::string jpeg_data;
                std::string mime_type;
                if (!board.GetLastCaptureJpeg(jpeg_data, &mime_type) || jpeg_data.empty()) {
                    throw std::runtime_error("No last camera capture is available yet");
                }
                return new ImageContent(mime_type.empty() ? "image/jpeg" : mime_type, jpeg_data);
            });
    }
#endif
}

void McpCameraToolsRegistrar::ParseCapabilities(McpServer& server, const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (!cJSON_IsObject(vision)) {
        return;
    }

    auto url = cJSON_GetObjectItem(vision, "url");
    auto token = cJSON_GetObjectItem(vision, "token");
    if (!cJSON_IsString(url) || url->valuestring == nullptr) {
        return;
    }

    server.vision_url_ = std::string(url->valuestring);
    server.vision_token_.clear();
    if (cJSON_IsString(token) && token->valuestring != nullptr) {
        server.vision_token_ = std::string(token->valuestring);
    }

    auto camera = Board::GetInstance().GetCamera();
    if (camera) {
        camera->SetExplainUrl(server.vision_url_, server.vision_token_);
    }
}
