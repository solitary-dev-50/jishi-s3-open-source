#ifndef _JISHI_CAMERA_PREVIEW_CONTROLLER_H_
#define _JISHI_CAMERA_PREVIEW_CONTROLLER_H_

#include "board.h"

#include <cstdint>
#include <functional>
#include <string>

#include <esp_http_server.h>

class MossCameraPreviewController {
public:
    using GetLastCaptureJpegCallback = std::function<bool(std::string&, std::string*)>;

    explicit MossCameraPreviewController(GetLastCaptureJpegCallback get_last_capture_jpeg);
    ~MossCameraPreviewController();

    void HandleNetworkEvent(NetworkEvent event);
    void Stop();

private:
    static esp_err_t HandlePreviewPageRequest(httpd_req_t* req);
    static esp_err_t HandlePreviewImageRequest(httpd_req_t* req);

    esp_err_t SendPreviewPage(httpd_req_t* req) const;
    esp_err_t SendPreviewImage(httpd_req_t* req) const;
    void Start();

    GetLastCaptureJpegCallback get_last_capture_jpeg_;
    httpd_handle_t server_ = nullptr;

    static constexpr uint16_t kServerPort = 8081;
    static constexpr uint16_t kServerCtrlPort = 32770;
};

#endif // _JISHI_CAMERA_PREVIEW_CONTROLLER_H_

