#include "weather.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "board.h"
#include "settings.h"

#define TAG "Weather"

namespace {

// 天气变得慢，20 分钟够勤了；没网或接口抽风就半分钟再试，别把人家打冒烟
constexpr int kOkIntervalSec = 20 * 60;
constexpr int kFailIntervalSec = 30;

// 人口低于这个数的「命中」基本是同名小村，加个「市」再问一次
constexpr long kMinCityPopulation = 500000;

std::string HttpGet(const std::string& url) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return "";
    }
    auto http = network->CreateHttp(0);
    if (!http) {
        return "";
    }
    http->SetTimeout(8000);
    http->SetHeader("User-Agent", "curl/8.0");
    if (!http->Open("GET", url)) {
        http->Close();
        ESP_LOGW(TAG, "Open failed: %s", url.c_str());
        return "";
    }
    int code = http->GetStatusCode();
    if (code != 200) {
        http->Close();
        ESP_LOGW(TAG, "HTTP %d: %s", code, url.c_str());
        return "";
    }
    std::string body = http->ReadAll();
    http->Close();
    return body;
}

// WMO 天气代码表。之前按英文描述猜子串（"partly cloudy" 之类），
// 措辞一变就漏判；代码是标准值，只查表。
const char* WmoDesc(int code) {
    switch (code) {
        case 0:
            return "晴";
        case 1:
            return "少云";
        case 2:
            return "多云";
        case 3:
            return "阴";
        case 45:
        case 48:
            return "雾";
        case 51:
        case 53:
        case 55:
            return "毛毛雨";
        case 56:
        case 57:
        case 66:
        case 67:
            return "冻雨";
        case 61:
            return "小雨";
        case 63:
            return "中雨";
        case 65:
            return "大雨";
        case 71:
            return "小雪";
        case 73:
            return "中雪";
        case 75:
            return "大雪";
        case 77:
            return "米雪";
        case 80:
            return "阵雨";
        case 81:
            return "强阵雨";
        case 82:
            return "暴雨";
        case 85:
            return "阵雪";
        case 86:
            return "强阵雪";
        case 95:
            return "雷阵雨";
        case 96:
        case 99:
            return "雷雨冰雹";
        default:
            return "";
    }
}

// 八方位。中文习惯先说「东北风」不说「65 度风」
const char* WindDir(double deg) {
    static const char* kDirs[] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};
    if (deg < 0) {
        return "";
    }
    int idx = static_cast<int>((deg + 22.5) / 45.0) % 8;
    return kDirs[idx];
}

// 蒲福风级上限（km/h）。查表比 pow(v, 2/3) 好懂，也省一次浮点幂
int BeaufortLevel(double kmh) {
    static const double kTop[] = {1, 5, 11, 19, 28, 38, 49, 61, 74, 88, 102, 117};
    for (int i = 0; i < 12; ++i) {
        if (kmh < kTop[i]) {
            return i;
        }
    }
    return 12;
}

// 美国 AQI 口径：100 以内算能放心呼吸
const char* AqiLevel(int aqi) {
    if (aqi <= 50) {
        return "优";
    }
    if (aqi <= 100) {
        return "良";
    }
    if (aqi <= 150) {
        return "轻度";
    }
    if (aqi <= 200) {
        return "中度";
    }
    return "重度";
}

bool Has(const char* s, const char* key) {
    return strstr(s, key) != nullptr;
}

// 一句话生活提示。按「对行动的影响」排优先级，只给最该听的那条 ——
// 屏上就这么一行，三条提示等于没有提示。
const char* MakeTip(const char* desc, int temp, int high, int low, int humidity, int wind_level,
                    int uv, int aqi) {
    if (Has(desc, "雷")) {
        return "打雷了，别在外久待";
    }
    if (Has(desc, "雨") || Has(desc, "雪")) {
        return "有雨雪，出门记得带伞";
    }
    if (aqi > 150) {
        return "空气差，出门戴个口罩";
    }
    if (temp <= 0) {
        return "零下了，路面可能结冰";
    }
    if (temp <= 8) {
        return "挺冷的，穿厚点";
    }
    if (high - low >= 10) {
        return "昼夜温差大，带件外套";
    }
    if (temp >= 33) {
        return "太热了，多补水";
    }
    if (uv >= 6) {
        return "紫外线强，防晒做足";
    }
    if (wind_level >= 5) {
        return "风有点大，注意防风";
    }
    if (humidity <= 30) {
        return "空气干，多喝水";
    }
    if (strcmp(desc, "晴") == 0 && temp >= 12 && temp <= 28) {
        return "天气不错，出去走走";
    }
    return "";
}

// 屏只有 360px 宽，"承德市"的行政后缀白占一个字的位置
void TrimCitySuffix(std::string& city) {
    static const char* kSuffixes[] = {"自治州", "地区", "市", "盟"};
    for (const char* suffix : kSuffixes) {
        size_t len = strlen(suffix);
        if (city.size() > len && city.compare(city.size() - len, len, suffix) == 0) {
            city.resize(city.size() - len);
            return;
        }
    }
}

// 中文地名要转 %XX 才能塞进 URL
std::string UrlEncode(const std::string& raw) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size() * 3);
    for (unsigned char c : raw) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

namespace {

struct GeoHit {
    bool ok = false;
    long population = 0;
    std::string name;
    std::string lat;
    std::string lon;
};

// 中文短名在 open-meteo 库里经常命中小村子（"保定" 只回云南一个村），
// 所以拿一批候选、只看中国、挑人口最多的那个。
GeoHit QueryGeo(const std::string& query) {
    GeoHit hit;
    char url[256];
    snprintf(url, sizeof(url),
             "http://geocoding-api.open-meteo.com/v1/search?name=%s&count=10&language=zh&format=json",
             UrlEncode(query).c_str());
    std::string body = HttpGet(url);
    if (body.empty()) {
        return hit;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return hit;
    }
    cJSON* results = cJSON_GetObjectItem(root, "results");
    if (cJSON_IsArray(results)) {
        int n = cJSON_GetArraySize(results);
        for (int i = 0; i < n; ++i) {
            cJSON* item = cJSON_GetArrayItem(results, i);
            cJSON* country = cJSON_GetObjectItem(item, "country_code");
            cJSON* lat = cJSON_GetObjectItem(item, "latitude");
            cJSON* lon = cJSON_GetObjectItem(item, "longitude");
            if (!cJSON_IsString(country) || strcmp(country->valuestring, "CN") != 0 ||
                !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
                continue;
            }
            cJSON* pop = cJSON_GetObjectItem(item, "population");
            long population = cJSON_IsNumber(pop) ? static_cast<long>(pop->valuedouble) : 0;
            if (hit.ok && population <= hit.population) {
                continue;
            }
            char lat_buf[16];
            char lon_buf[16];
            snprintf(lat_buf, sizeof(lat_buf), "%.4f", lat->valuedouble);
            snprintf(lon_buf, sizeof(lon_buf), "%.4f", lon->valuedouble);
            cJSON* name = cJSON_GetObjectItem(item, "name");
            hit.ok = true;
            hit.population = population;
            hit.name = cJSON_IsString(name) ? name->valuestring : query;
            hit.lat = lat_buf;
            hit.lon = lon_buf;
        }
    }
    cJSON_Delete(root);
    return hit;
}

}  // namespace

Weather& Weather::GetInstance() {
    static Weather instance;
    return instance;
}

void Weather::Start() {
    if (task_ != nullptr) {
        return;
    }
    Load();
    xTaskCreate(&Weather::TaskEntry, "weather", 8192, this, 3, &task_);
}

std::string Weather::Text() {
    std::lock_guard<std::mutex> lock(mutex_);
    return text_;
}

std::string Weather::Detail() {
    std::lock_guard<std::mutex> lock(mutex_);
    return detail_;
}

void Weather::TaskEntry(void* arg) {
    static_cast<Weather*>(arg)->Loop();
}

void Weather::Load() {
    Settings settings("weather", false);
    std::lock_guard<std::mutex> lock(mutex_);
    city_ = settings.GetString("city");
    lat_text_ = settings.GetString("lat");
    lon_text_ = settings.GetString("lon");
    // 城市名和坐标是两个来源，靠这行才能看出它们到底对得上不对
    ESP_LOGI(TAG, "Loaded city=%s lat=%s lon=%s", city_.c_str(), lat_text_.c_str(),
             lon_text_.c_str());
}

void Weather::Loop() {
    // 等 Wi-Fi 起来，别一上电就去撞没网的 CreateHttp
    vTaskDelay(pdMS_TO_TICKS(5000));
    while (true) {
        bool ok = Refresh();
        int sec = ok ? kOkIntervalSec : kFailIntervalSec;
        // 按秒等而不是一口气 sleep 20 分钟：用户刚换完城市要马上看到结果
        for (int i = 0; i < sec; ++i) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (refresh_requested_.exchange(false)) {
                break;
            }
        }
    }
}

bool Weather::Locate() {
    // 中文城市名只有 ip-api 给得出来，open-meteo 的 geocode 库反查是英文
    std::string body = HttpGet("http://ip-api.com/json/?lang=zh-CN&fields=status,city,lat,lon");
    if (body.empty()) {
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }

    bool ok = false;
    cJSON* status = cJSON_GetObjectItem(root, "status");
    cJSON* city = cJSON_GetObjectItem(root, "city");
    cJSON* lat = cJSON_GetObjectItem(root, "lat");
    cJSON* lon = cJSON_GetObjectItem(root, "lon");
    if (cJSON_IsString(status) && strcmp(status->valuestring, "success") == 0 &&
        cJSON_IsString(city) && cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
        std::string name = city->valuestring;
        TrimCitySuffix(name);
        if (!name.empty()) {
            char lat_buf[16];
            char lon_buf[16];
            snprintf(lat_buf, sizeof(lat_buf), "%.4f", lat->valuedouble);
            snprintf(lon_buf, sizeof(lon_buf), "%.4f", lon->valuedouble);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                city_ = name;
                lat_text_ = lat_buf;
                lon_text_ = lon_buf;
            }
            Settings settings("weather", true);
            settings.SetString("city", name);
            settings.SetString("lat", lat_buf);
            settings.SetString("lon", lon_buf);
            ESP_LOGI(TAG, "Located: %s (%s, %s)", name.c_str(), lat_buf, lon_buf);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

bool Weather::Geocode(const std::string& name, std::string& lat, std::string& lon) {
    GeoHit hit = QueryGeo(name);
    // 「保定」→云南一个村（无人口），「保定市」→河北保定 273 万人。
    // 命中人口不够大就补个「市」再问一次，两次取人口多的
    if (!hit.ok || hit.population < kMinCityPopulation) {
        GeoHit with_city = QueryGeo(name + "市");
        if (with_city.ok && (!hit.ok || with_city.population > hit.population)) {
            hit = with_city;
        }
    }
    if (!hit.ok) {
        ESP_LOGW(TAG, "Geocode failed: %s", name.c_str());
        return false;
    }
    lat = hit.lat;
    lon = hit.lon;
    ESP_LOGI(TAG, "Geocoded %s -> %s,%s (pop %ld)", name.c_str(), lat.c_str(), lon.c_str(),
             hit.population);
    return true;
}

bool Weather::RefreshAir(const std::string& lat, const std::string& lon) {
    char url[256];
    snprintf(url, sizeof(url),
             "http://air-quality-api.open-meteo.com/v1/air-quality?latitude=%s&longitude=%s"
             "&current=pm2_5,us_aqi&timezone=auto",
             lat.c_str(), lon.c_str());
    std::string body = HttpGet(url);
    int aqi = -1;
    if (!body.empty()) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            cJSON* cur = cJSON_GetObjectItem(root, "current");
            cJSON* v = cJSON_IsObject(cur) ? cJSON_GetObjectItem(cur, "us_aqi") : nullptr;
            if (cJSON_IsNumber(v)) {
                aqi = static_cast<int>(lround(v->valuedouble));
            }
            cJSON_Delete(root);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // 拉不到就留着上次的值：AQI 一小时才变一次，清空不如留着
    if (aqi > 0) {
        aqi_ = aqi;
    }
    return aqi > 0;
}

bool Weather::Refresh() {
    std::string city;
    std::string lat_text;
    std::string lon_text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        city = city_;
        lat_text = lat_text_;
        lon_text = lon_text_;
    }

    // 只要没城市就去定位；用户手动报的地名不能拿 IP 坐标盖掉
    if (city.empty()) {
        if (!Locate()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        city = city_;
        lat_text = lat_text_;
        lon_text = lon_text_;
    }

    // 手报的地名没有坐标：先 geocode。拿到坐标之后就和 IP 定位走同一条路
    if (lat_text.empty() || lon_text.empty()) {
        std::string lat;
        std::string lon;
        if (!Geocode(city, lat, lon)) {
            return false;
        }
        lat_text = lat;
        lon_text = lon;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lat_text_ = lat;
            lon_text_ = lon;
        }
        Settings settings("weather", true);
        settings.SetString("lat", lat);
        settings.SetString("lon", lon);
    }

    char url[384];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,"
             "wind_speed_10m,wind_direction_10m"
             "&daily=temperature_2m_max,temperature_2m_min,uv_index_max,sunset"
             "&timezone=auto&forecast_days=1",
             lat_text.c_str(), lon_text.c_str());
    std::string body = HttpGet(url);
    if (body.empty()) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Bad JSON (%u bytes): %.60s", (unsigned)body.size(), body.c_str());
        return false;
    }

    auto num = [](cJSON* obj, const char* key, double* out) {
        cJSON* v = cJSON_GetObjectItem(obj, key);
        if (!cJSON_IsNumber(v)) {
            return false;
        }
        *out = v->valuedouble;
        return true;
    };
    // daily 里的值是长度 1 的数组，永远取第 0 个（今天是 forecast_days=1）
    auto day0 = [](cJSON* obj, const char* key, double* out) {
        cJSON* arr = cJSON_GetObjectItem(obj, key);
        if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 1) {
            return false;
        }
        cJSON* v = cJSON_GetArrayItem(arr, 0);
        if (!cJSON_IsNumber(v)) {
            return false;
        }
        *out = v->valuedouble;
        return true;
    };
    // 日落跟温度不同：是字符串，得单独取。取不到就留空，屏上显示「—」
    auto day_str0 = [](cJSON* obj, const char* key, std::string& out) {
        cJSON* arr = cJSON_GetObjectItem(obj, key);
        if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 1) {
            return false;
        }
        cJSON* v = cJSON_GetArrayItem(arr, 0);
        if (!cJSON_IsString(v)) {
            return false;
        }
        out = v->valuestring;
        return true;
    };
    cJSON* cur = cJSON_GetObjectItem(root, "current");
    cJSON* daily = cJSON_GetObjectItem(root, "daily");
    double temp = 0;
    double feels = 0;
    double humidity = 0;
    double code = 0;
    double wind_speed = 0;
    double wind_deg = 0;
    double high = 0;
    double low = 0;
    double uv = 0;
    bool ok = cJSON_IsObject(cur) && cJSON_IsObject(daily) &&
              num(cur, "temperature_2m", &temp) && num(cur, "apparent_temperature", &feels) &&
              num(cur, "relative_humidity_2m", &humidity) && num(cur, "weather_code", &code) &&
              num(cur, "wind_speed_10m", &wind_speed) &&
              num(cur, "wind_direction_10m", &wind_deg) &&
              day0(daily, "temperature_2m_max", &high) &&
              day0(daily, "temperature_2m_min", &low) && day0(daily, "uv_index_max", &uv);
    std::string sunset_iso;
    day_str0(daily, "sunset", sunset_iso);
    if (!ok) {
        ESP_LOGW(TAG, "Missing fields (%u bytes): %.80s", (unsigned)body.size(), body.c_str());
        cJSON_Delete(root);
        return false;
    }
    cJSON_Delete(root);

    int temp_i = static_cast<int>(lround(temp));
    int feels_i = static_cast<int>(lround(feels));
    int high_i = static_cast<int>(lround(high));
    int low_i = static_cast<int>(lround(low));
    int humidity_i = static_cast<int>(lround(humidity));
    int uv_i = static_cast<int>(lround(uv));
    int wind_level = BeaufortLevel(wind_speed);
    const char* desc = WmoDesc(static_cast<int>(lround(code)));
    const char* wind_dir = WindDir(wind_deg);

    // 日落 "2026-10-30T17:20" 只留 "17:20"
    std::string sunset;
    size_t t_pos = sunset_iso.find('T');
    if (t_pos != std::string::npos && sunset_iso.size() >= t_pos + 6) {
        sunset = sunset_iso.substr(t_pos + 1, 5);
    }


    RefreshAir(lat_text, lon_text);
    int aqi;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        aqi = aqi_;
    }

    char buf[96];
    // 单行摘要给 MCP / AI 播报用
    std::string text = city;
    if (desc[0] != '\0') {
        text += " ";
        text += desc;
    }
    snprintf(buf, sizeof(buf), " %d°C，体感 %d°C，今天 %d~%d°C", temp_i, feels_i, low_i, high_i);
    text += buf;
    if (aqi > 0) {
        snprintf(buf, sizeof(buf), "，空气%s", AqiLevel(aqi));
        text += buf;
    }

    // 屏只有 240x240（内容区 216），20px 字体一行约 11 个汉字，可用宽度 230px。
    // HUD 起点 140 给 3 行 72px：城市/温区/提示不滚就看得到，剩下两行走慢滚

    std::string detail;
    snprintf(buf, sizeof(buf), "%s %s %d° 体感%d°", city.c_str(),
             desc[0] != '\0' ? desc : "—", temp_i, feels_i);
    detail = buf;
    snprintf(buf, sizeof(buf), "%d~%d° 湿%d%% %s风%d级", low_i, high_i, humidity_i, wind_dir,
             wind_level);
    detail += "\n";
    detail += buf;
    // 第 3 行：有提示就是那一句话——冷热、紫外、空气质量这些判断已经包在里面了
    const char* tip = MakeTip(desc, temp_i, high_i, low_i, humidity_i, wind_level, uv_i, aqi);
    if (tip[0] != '\0') {
        detail += "\n";
        detail += tip;
    }
    // 第 4/5 行：具体数字，提示没提到的也能查到。「紫外N 空气XX 日落HH:MM」实测
    // 234px > 可用 230px，会被折成半行——拆开各占一行，交给视窗滚动，信息一条不丢
    snprintf(buf, sizeof(buf), "紫外%d 空气%s", uv_i, aqi > 0 ? AqiLevel(aqi) : "—");
    detail += "\n";
    detail += buf;
    if (!sunset.empty()) {
        snprintf(buf, sizeof(buf), "日落 %s", sunset.c_str());
        detail += "\n";
        detail += buf;
    }


    {
        std::lock_guard<std::mutex> lock(mutex_);
        text_ = text;
        detail_ = detail;
    }
    ESP_LOGI(TAG, "Now: %s | raw temp=%.1f feels=%.1f code=%d wind=%.1fkm/h@%.0f uv=%.1f", text.c_str(),
             temp, feels, (int)lround(code), wind_speed, wind_deg, uv);
    // 屏上每一行都得留在这条日志里：折行会让行数超标去滚动，出事时先看这里
    ESP_LOGI(TAG, "HUD text: [%s]", detail.c_str());
    return true;
}

std::string Weather::SetCity(const std::string& city) {
    std::string name = city;
    TrimCitySuffix(name);
    if (name.empty()) {
        return "{\"success\": false, \"message\": \"没说城市名\"}";
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        city_ = name;
        // 旧坐标是旧城市的，留着不删下一次 Refresh 又按它查回去了
        lat_text_.clear();
        lon_text_.clear();
        text_.clear();
        detail_.clear();
        aqi_ = -1;
    }
    Settings settings("weather", true);
    settings.SetString("city", name);
    settings.SetString("lat", "");
    settings.SetString("lon", "");
    ESP_LOGI(TAG, "City set to %s, waiting for refresh", name.c_str());

    refresh_requested_.exchange(true);
    // 等真拉到天气再回话，否则 AI 会报一个屏幕上根本没有的城市
    for (int i = 0; i < 12; ++i) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        std::lock_guard<std::mutex> lock(mutex_);
        if (!text_.empty()) {
            return "{\"success\": true, \"city\": \"" + name + "\", \"now\": \"" + text_ + "\"}";
        }
    }
    return "{\"success\": false, \"message\": \"没查到「" + name +
           "」的天气，地名可能不对\"}";
}
