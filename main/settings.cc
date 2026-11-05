#include "settings.h"

#include <esp_cpu.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include <mutex>

#define TAG "Settings"

namespace {

// 栈静态分配在 .bss（一定在内部 RAM），写 flash 才不会被 cache 断言拦下。
// 单位是 StackType_t（4 字节）：3072 字 = 12KB，够 nvs_set 那点调用链，
// 别按字节数填——8192 会白吃 32KB 内部 RAM（2.0.17 只剩 37KB free sram）。
constexpr size_t kNvsWorkerStackDepth = 3072;

struct NvsJob {
    const std::function<void()>* fn;
    SemaphoreHandle_t done;
};

QueueHandle_t queue_ = nullptr;
StaticTask_t worker_tcb_;
StackType_t worker_stack_[kNvsWorkerStackDepth];
std::mutex start_mutex_;

void NvsWorker(void*) {
    NvsJob job;
    while (xQueueReceive(queue_, &job, portMAX_DELAY) == pdTRUE) {
        (*job.fn)();
        xSemaphoreGive(job.done);
    }
    vTaskDelete(nullptr);
}

bool StartWorkerLocked() {
    if (queue_ != nullptr) {
        return true;
    }
    auto queue = xQueueCreate(4, sizeof(NvsJob));
    if (queue == nullptr) {
        return false;
    }
    // 句柄必须先发布再起任务：xTaskCreateStatic 会让 worker 立刻抢跑，
    // 它第一句就是 xQueueReceive(queue_)，晚赋值一步就是 assert (pxQueue)。
    queue_ = queue;
    if (xTaskCreateStatic(NvsWorker, "nvs_write", kNvsWorkerStackDepth, nullptr, 5, worker_stack_,
                          &worker_tcb_) == nullptr) {
        queue_ = nullptr;
        vQueueDelete(queue);
        return false;
    }
    return true;
}

}  // namespace

void RunNvsWrite(const std::function<void()>& fn) {
    // 栈已经在内部 RAM：直接写，省一次任务切换
    if (esp_ptr_in_dram(esp_cpu_get_sp())) {
        fn();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(start_mutex_);
        if (!StartWorkerLocked()) {
            ESP_LOGE(TAG, "no memory for NVS worker; refusing unsafe flash write");
            return;
        }
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == nullptr) {
        ESP_LOGE(TAG, "no semaphore for NVS write; refusing unsafe flash write");
        return;
    }
    NvsJob job{&fn, done};
    if (xQueueSend(queue_, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "NVS write queue full, dropped");
        vSemaphoreDelete(done);
        return;
    }
    if (xSemaphoreTake(done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "NVS write timeout");
    }
    vSemaphoreDelete(done);
}

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    RunNvsWrite([this]() {
        nvs_open(ns_.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
    });
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
        if (read_write_ && dirty_) {
            // commit 才是真正落 flash 的那一步，同样不能在 PSRAM 栈上跑
            RunNvsWrite([this]() { ESP_ERROR_CHECK(nvs_commit(nvs_handle_)); });
        }
        nvs_close(nvs_handle_);
    }
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    size_t length = 0;
    if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
        return default_value;
    }

    std::string value;
    value.resize(length);
    ESP_ERROR_CHECK(nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length));
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

void Settings::SetString(const std::string& key, const std::string& value) {
    if (read_write_) {
        std::string k = key;
        std::string v = value;
        RunNvsWrite([this, k, v]() {
            ESP_ERROR_CHECK(nvs_set_str(nvs_handle_, k.c_str(), v.c_str()));
            dirty_ = true;
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }

    int32_t value;
    if (nvs_get_i32(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value;
}

void Settings::SetInt(const std::string& key, int32_t value) {
    if (read_write_) {
        RunNvsWrite([this, key, value]() {
            ESP_ERROR_CHECK(nvs_set_i32(nvs_handle_, key.c_str(), value));
            dirty_ = true;
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseKey(const std::string& key) {
    if (read_write_) {
        RunNvsWrite([this, key]() {
            auto ret = nvs_erase_key(nvs_handle_, key.c_str());
            if (ret != ESP_ERR_NVS_NOT_FOUND) {
                ESP_ERROR_CHECK(ret);
            }
            dirty_ = true;
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseAll() {
    if (read_write_) {
        RunNvsWrite([this]() {
            ESP_ERROR_CHECK(nvs_erase_all(nvs_handle_));
            dirty_ = true;
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}
