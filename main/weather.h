#ifndef WEATHER_H
#define WEATHER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <mutex>
#include <string>

/**
 * 待机屏上那几行天气。固件自己去公网拉，服务端掉线、WS 没连也照显示。
 *
 * 三个免 key 的 http 接口（都支持明文 http，省掉 TLS 证书那一套）：
 * ip-api 给中文城市名 + 经纬度，open-meteo 按坐标给实况 + 今日高低温 + 日落，
 * air-quality-api 给 PM2.5。
 *
 * 定位有两种模式：默认 auto——每次刷新都按出口 IP 重新定位，设备换网络、换地方
 * 城市就自己跟着变；用户报过地名之后变 manual，IP 再也盖不掉它。
 */
class Weather {
public:
    static Weather& GetInstance();

    void Start();
    // 一行摘要，如 "保定 晴 13°C，体感 8°C，今天 9~23°C"；还没拿到就是空串
    std::string Text();
    // 待机屏那几行（含生活提示），行间用 \n 分隔
    std::string Detail();
    // 换城市（用户报地名）。等它真拉到天气才返回 success，拉不到就报错回去
    std::string SetCity(const std::string& city);
    // 丢掉手动设定的城市，回到「跟着出口 IP 走」的模式
    std::string UseIpLocation();

private:
    Weather() = default;

    static void TaskEntry(void* arg);
    void Loop();
    bool Refresh();
    bool RefreshAir(const std::string& lat, const std::string& lon);
    bool Locate();
    bool Geocode(const std::string& name, std::string& lat, std::string& lon);
    void Load();

    std::mutex mutex_;
    std::string text_;
    std::string detail_;
    std::string city_;
    std::string lat_text_;
    std::string lon_text_;
    bool manual_ = false;  // 城市是用户报的还是 IP 定位来的；手动的 IP 不许盖
    int aqi_ = -1;  // -1 = 这个位置拿不到空气质量
    std::atomic<bool> refresh_requested_{false};
    TaskHandle_t task_ = nullptr;
};

#endif
