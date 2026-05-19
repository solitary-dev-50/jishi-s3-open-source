#include "jishi_camera_preview_controller.h"

#include <esp_log.h>
#include <wifi_manager.h>

namespace {
constexpr const char* TAG = "MossCamPreview";
}

MossCameraPreviewController::MossCameraPreviewController(GetLastCaptureJpegCallback get_last_capture_jpeg)
    : get_last_capture_jpeg_(std::move(get_last_capture_jpeg)) {}

MossCameraPreviewController::~MossCameraPreviewController() {
    Stop();
}

void MossCameraPreviewController::HandleNetworkEvent(NetworkEvent event) {
    switch (event) {
        case NetworkEvent::Connected:
            Start();
            break;
        case NetworkEvent::Disconnected:
        case NetworkEvent::WifiConfigModeEnter:
            Stop();
            break;
        default:
            break;
    }
}

void MossCameraPreviewController::Start() {
    if (server_ != nullptr) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kServerPort;
    config.ctrl_port = kServerCtrlPort;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 6144;

    if (httpd_start(&server_, &config) != ESP_OK) {
        server_ = nullptr;
        ESP_LOGE(TAG, "Failed to start camera preview server on port %u",
                 static_cast<unsigned>(kServerPort));
        return;
    }

    httpd_uri_t preview_page_uri = {
        .uri = "/camera",
        .method = HTTP_GET,
        .handler = &MossCameraPreviewController::HandlePreviewPageRequest,
        .user_ctx = this,
    };
    httpd_uri_t preview_image_uri = {
        .uri = "/camera.jpg",
        .method = HTTP_GET,
        .handler = &MossCameraPreviewController::HandlePreviewImageRequest,
        .user_ctx = this,
    };

    const esp_err_t page_result = httpd_register_uri_handler(server_, &preview_page_uri);
    const esp_err_t image_result = httpd_register_uri_handler(server_, &preview_image_uri);
    if (page_result != ESP_OK || image_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register camera preview routes: page=%s image=%s",
                 esp_err_to_name(page_result),
                 esp_err_to_name(image_result));
        httpd_stop(server_);
        server_ = nullptr;
        return;
    }

    const std::string ip_address = WifiManager::GetInstance().GetIpAddress();
    ESP_LOGI(TAG, "Camera preview ready: http://%s:%u/camera",
             ip_address.empty() ? "<ip-unavailable>" : ip_address.c_str(),
             static_cast<unsigned>(kServerPort));
}

void MossCameraPreviewController::Stop() {
    if (server_ == nullptr) {
        return;
    }
    httpd_stop(server_);
    server_ = nullptr;
    ESP_LOGI(TAG, "Camera preview server stopped");
}

esp_err_t MossCameraPreviewController::HandlePreviewPageRequest(httpd_req_t* req) {
    auto* self = static_cast<MossCameraPreviewController*>(req->user_ctx);
    return self != nullptr ? self->SendPreviewPage(req) : ESP_FAIL;
}

esp_err_t MossCameraPreviewController::HandlePreviewImageRequest(httpd_req_t* req) {
    auto* self = static_cast<MossCameraPreviewController*>(req->user_ctx);
    return self != nullptr ? self->SendPreviewImage(req) : ESP_FAIL;
}

esp_err_t MossCameraPreviewController::SendPreviewPage(httpd_req_t* req) const {
    static constexpr const char* kPreviewPageHtml = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Ji Shi Camera Preview</title>
  <style>
    :root {
      color-scheme: light;
      font-family: "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif;
      background: #f4f4f1;
      color: #1e1f1c;
    }
    body {
      margin: 0;
      padding: 20px;
      background:
        radial-gradient(circle at top left, rgba(146, 181, 126, 0.22), transparent 36%),
        linear-gradient(180deg, #f8f7f2 0%, #ece8df 100%);
    }
    main {
      max-width: 980px;
      margin: 0 auto;
      display: grid;
      gap: 14px;
    }
    h1 {
      margin: 0;
      font-size: 24px;
      letter-spacing: 0.02em;
    }
    .hint {
      margin: 0;
      color: #5a5e55;
      line-height: 1.5;
    }
    .toolbar {
      display: flex;
      gap: 10px;
      flex-wrap: wrap;
      align-items: center;
    }
    button {
      border: 0;
      border-radius: 999px;
      padding: 10px 16px;
      background: #275d38;
      color: #fff;
      font-size: 15px;
      cursor: pointer;
    }
    #status {
      color: #7a3022;
      font-size: 14px;
    }
    .frame {
      padding: 14px;
      border-radius: 20px;
      background: rgba(255, 255, 255, 0.86);
      box-shadow: 0 12px 40px rgba(27, 34, 24, 0.12);
    }
    img {
      display: block;
      width: 100%;
      height: auto;
      border-radius: 12px;
      background: #d9d5cc;
      min-height: 220px;
      object-fit: contain;
    }
  </style>
</head>
<body>
  <main>
    <div>
      <h1>最近一张抓拍</h1>
      <p class="hint">先让设备拍照，再点刷新。这个页面只显示板子里最近一次保存的 JPEG。</p>
    </div>
    <div class="toolbar">
      <button type="button" onclick="reloadPreview()">刷新图片</button>
      <span id="status">等待加载...</span>
    </div>
    <div class="frame">
      <img id="preview" alt="最近一张抓拍">
    </div>
  </main>
  <script>
    const preview = document.getElementById('preview');
    const status = document.getElementById('status');

    function reloadPreview() {
      status.textContent = '加载中...';
      preview.onload = function () {
        status.textContent = '已加载最近一张抓拍';
      };
      preview.onerror = function () {
        status.textContent = '还没有抓拍图片，请先让设备拍一次照后再刷新。';
      };
      preview.src = '/camera.jpg?t=' + Date.now();
    }

    reloadPreview();
  </script>
</body>
</html>
)HTML";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, kPreviewPageHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t MossCameraPreviewController::SendPreviewImage(httpd_req_t* req) const {
    std::string jpeg_data;
    std::string mime_type;
    if (!get_last_capture_jpeg_ || !get_last_capture_jpeg_(jpeg_data, &mime_type) || jpeg_data.empty()) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
        return httpd_resp_sendstr(req, "No captured image is available yet.");
    }

    httpd_resp_set_type(req, mime_type.c_str());
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, jpeg_data.data(), jpeg_data.size());
}

