/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

 #include "mcp_server.h"
 #include <esp_log.h>
 #include <esp_app_desc.h>
 #include <algorithm>
 #include <cstring>
 #include <cctype>
 #include <cstdlib>
 #include <cstdio>
 #include <esp_pthread.h>
 
 #include "application.h"
 #include "display.h"
 #include "board.h"
 #include "boards/common/esp32_music.h"
 #include "alarm.h"
#include "weather.h"
 
 #define TAG "MCP"
 
  #define DEFAULT_TOOLCALL_STACK_SIZE 6144
  
  // 「工作日」「周末」「1,3,5」→ 星期位掩码（bit0=周日，对齐 tm_wday）；说不清就返回 -1
  static int ParseWeekdays(const std::string& text) {
      if (text.empty()) {
          return 0;
      }
      if (text == "daily" || text == "everyday" || text == "all" || text == "每天") {
          return 0x7F;
      }
      if (text == "workday" || text == "weekday" || text == "工作日" || text == "平时") {
          return 0x3E;
      }
      if (text == "weekend" || text == "周末") {
          return 0x41;
      }
      if (text == "once" || text == "一次" || text == "0") {
          return 0;
      }
      int mask = 0;
      bool any = false;
      const char* p = text.c_str();
      while (*p) {
          while (*p == ',' || *p == ' ' || *p == '/') {
              ++p;
          }
          if (*p < '0' || *p > '6') {
              return -1;
          }
          mask |= 1 << (*p - '0');
          any = true;
          while (*p >= '0' && *p <= '6') {
              ++p;
          }
      }
      return any ? mask : -1;
  }
 
 // 色名 / #RRGGBB → RGB。中文名允许带「色」尾巴（深蓝色、浅绿都认）
static bool ParseColorName(const std::string& raw, uint32_t* rgb) {
    size_t b = raw.find_first_not_of(" \t");
    if (b == std::string::npos) {
        return false;
    }
    size_t e = raw.find_last_not_of(" \t");
    std::string name = raw.substr(b, e - b + 1);
    if (name[0] == '#') {
        char* end = nullptr;
        unsigned long v = strtoul(name.c_str() + 1, &end, 16);
        if (end == name.c_str() + 1 || v == 0 || v > 0xFFFFFF) {
            return false;
        }
        *rgb = (uint32_t)v;
        return true;
    }
    static const struct { const char* key; uint32_t rgb; } kColors[] = {
        {"粉", 0xFF8FB8}, {"粉色", 0xFF8FB8}, {"pink", 0xFF8FB8},
        {"金", 0xFFD76B}, {"金色", 0xFFD76B}, {"gold", 0xFFD76B},
        {"银", 0xE0E6F0}, {"银灰", 0xE0E6F0}, {"silver", 0xE0E6F0},
        {"白", 0xF2F6FF}, {"white", 0xF2F6FF},
        {"黑", 0x2A2A38}, {"black", 0x2A2A38},
        {"紫", 0xB48CFF}, {"purple", 0xB48CFF},
        {"深紫", 0x7A4BD6}, {"darkpurple", 0x7A4BD6},
        {"绿", 0x8CF0B4}, {"绿色", 0x8CF0B4}, {"green", 0x8CF0B4},
        {"浅绿", 0x7BF5B0}, {"lightgreen", 0x7BF5B0},
        {"深绿", 0x1FA35C}, {"darkgreen", 0x1FA35C},
        {"红", 0xFF6B6B}, {"red", 0xFF6B6B},
        {"深红", 0xB3241F}, {"darkred", 0xB3241F},
        {"棕", 0xB07A50}, {"brown", 0xB07A50},
        {"蓝", 0x76A6FF}, {"蓝色", 0x76A6FF}, {"blue", 0x76A6FF},
        {"浅蓝", 0x9BD4FF}, {"lightblue", 0x9BD4FF},
        {"深蓝", 0x2D5BFF}, {"darkblue", 0x2D5BFF},
        {"品红", 0xD8408C}, {"洋红", 0xD8408C}, {"magenta", 0xD8408C},
        {"青", 0x00E5FF}, {"青色", 0x00E5FF}, {"cyan", 0x00E5FF},
        {"橙", 0xFF9A3C}, {"橙色", 0xFF9A3C}, {"orange", 0xFF9A3C},
        {"黄", 0xFFE066}, {"黄色", 0xFFE066}, {"yellow", 0xFFE066},
    };
    const size_t n = sizeof(kColors) / sizeof(kColors[0]);
    for (size_t i = 0; i < n; i++) {
        if (name == kColors[i].key) {
            *rgb = kColors[i].rgb;
            return true;
        }
    }
    // 「深蓝色」这类比表里多一个「色」字，去掉再查一次（「色」是 3 字节）
    if (name.size() > 3 && name.compare(name.size() - 3, 3, "色") == 0) {
        std::string trimmed = name.substr(0, name.size() - 3);
        for (size_t i = 0; i < n; i++) {
            if (trimmed == kColors[i].key) {
                *rgb = kColors[i].rgb;
                return true;
            }
        }
    }
    return false;
}

McpServer::McpServer() {
 }
 
 McpServer::~McpServer() {
     for (auto tool : tools_) {
         delete tool;
     }
     tools_.clear();
 }
 
 void McpServer::AddCommonTools() {
     // To speed up the response time, we add the common tools to the beginning of
     // the tools list to utilize the prompt cache.
     // Backup the original tools list and restore it after adding the common tools.
     auto original_tools = std::move(tools_);
     auto& board = Board::GetInstance();
 
     AddTool("self.get_device_status",
         "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
         "Use this tool for: \n"
         "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
         "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
         PropertyList(),
         [&board](const PropertyList& properties) -> ReturnValue {
             return board.GetDeviceStatusJson();
         });
 
     AddTool("self.audio_speaker.set_volume", 
         "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
         PropertyList({
             Property("volume", kPropertyTypeInteger, 0, 100)
         }), 
         [&board](const PropertyList& properties) -> ReturnValue {
             auto codec = board.GetAudioCodec();
             codec->SetOutputVolume(properties["volume"].value<int>());
             return true;
         });
     
     auto backlight = board.GetBacklight();
     if (backlight) {
         AddTool("self.screen.set_brightness",
             "Set the brightness of the screen.",
             PropertyList({
                 Property("brightness", kPropertyTypeInteger, 0, 100)
             }),
             [backlight](const PropertyList& properties) -> ReturnValue {
                 uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                 backlight->SetBrightness(brightness, true);
                 return true;
             });
     }
 
    auto display = board.GetDisplay();
    if (display && !display->GetTheme().empty()) {
        AddTool("self.screen.set_theme",
             "Set the theme of the screen. The theme can be `light` or `dark`.",
             PropertyList({
                 Property("theme", kPropertyTypeString)
             }),
             [display](const PropertyList& properties) -> ReturnValue {
                 display->SetTheme(properties["theme"].value<std::string>().c_str());
                 return true;
             });
     }

    if (display) {
        AddTool("self.appearance.set_hair_color",
            "换屏幕上那个头像的头发颜色。用户说「头发换成粉色」「染成金色」时用它。\n"
            "参数:\n"
            "  `color`: 颜色，可以给中文/英文色名（粉/金/银/黑/白/紫/绿/红/棕/蓝），也可以给 #RRGGBB。\n"
            "返回 success 表示屏幕上已经是新发色，照着说就行。",
            PropertyList({
                Property("color", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                uint32_t rgb = 0;
                if (!ParseColorName(properties["color"].value<std::string>(), &rgb)) {
                    return std::string("{\"success\": false, \"message\": \"认不出这个颜色，给色名（粉/金/银/黑/白/紫/绿/红/棕/蓝）或 #RRGGBB\"}");
                }
                display->SetHairColor(rgb);
                char buf[80];
                snprintf(buf, sizeof(buf), "{\"success\": true, \"message\": \"头发已换成 #%06X\"}", (unsigned)rgb);
                return std::string(buf);
            });

        AddTool("self.screen.set_accent_color",
            "换屏幕主色，并控制整屏染色浓度。整屏（包括待机背景那张底图）都会染上这个颜色。\n"
            "用户说「背景改成深蓝色」「换成浅绿色」「整个屏幕变个气氛」「颜色太扎眼了/太暗了」时用它。\n"
            "参数:\n"
            "  `color`: 色名（品红/青/深蓝/浅蓝/深绿/浅绿/紫/金/橙/红/粉/黄/白），也可以给 #RRGGBB。\n"
            "  `strength`: 1~10 染色浓淡，默认 3。用户嫌看不出来就调到 6~8，喊太浓太糊就降到 1~2，0 = 只换顶栏和光效、不染整屏。\n"
            "返回 success 表示屏幕已经变色，照着说就行。",
            PropertyList({
                Property("color", kPropertyTypeString),
                Property("strength", kPropertyTypeInteger, 3, 0, 10)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                uint32_t rgb = 0;
                if (!ParseColorName(properties["color"].value<std::string>(), &rgb)) {
                    return std::string("{\"success\": false, \"message\": \"认不出这个颜色，给色名（品红/青/深蓝/浅蓝/深绿/浅绿/紫/金/橙/红/粉/黄/白）或 #RRGGBB\"}");
                }
                int strength = properties["strength"].value<int>();
                display->SetAccentColor(rgb, strength);
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "{\"success\": true, \"message\": \"屏幕主色已换成 #%06X，整屏染色 %d/10\"}",
                         (unsigned)rgb, strength);
                return std::string(buf);
            });
    }

 
     auto camera = board.GetCamera();
     if (camera) {
         AddTool("self.camera.take_photo",
             "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
             "Args:\n"
             "  `question`: The question that you want to ask about the photo.\n"
             "Return:\n"
             "  A JSON object that provides the photo information.",
             PropertyList({
                 Property("question", kPropertyTypeString)
             }),
             [camera](const PropertyList& properties) -> ReturnValue {
                 if (!camera->Capture()) {
                     return "{\"success\": false, \"message\": \"Failed to capture photo\"}";
                 }
                 auto question = properties["question"].value<std::string>();
                 return camera->Explain(question);
             });
     }
 
     auto music = board.GetMusic();
     if (music) {
        AddTool("self.music.play_song",
            "播放一首具体的歌（歌名或歌手明确时用）。用户说“听广播/电台/随便听听/放点背景音乐”时不要用它，改调 self.music.play_radio。\n"
            "参数:\n"
            "  `audio_url`: 可直连的音频直链（有就传，最快）。\n"
            "  `lyric_text`: LRC 歌词正文（强烈建议传）—— 设备本地解析，不必访问任何地址。\n"
            "  `lyric_url`: LRC 歌词地址（可选，仅当服务端和设备同在局域网时才连得上）。\n"
            "  `title`: 歌名（可选，屏幕显示与歌词同步用）。\n"
            "  `artist`: 歌手（可选）。\n"
            "  `song_name`: 歌名（没传 audio_url 时必填，设备自己搜曲拿直链）。\n"
            "返回:\n"
            "  播放状态信息，不需确认，立刻播放歌曲。",
            PropertyList({
                Property("audio_url", kPropertyTypeString, ""),
                Property("lyric_text", kPropertyTypeString, ""),
                Property("lyric_url", kPropertyTypeString, ""),
                Property("title", kPropertyTypeString, ""),
                Property("artist", kPropertyTypeString, ""),
                Property("song_name", kPropertyTypeString, "")
            }),
            [music](const PropertyList& properties) -> ReturnValue {
                auto audio_url = properties["audio_url"].value<std::string>();
                auto lyric_text = properties["lyric_text"].value<std::string>();
                auto lyric_url = properties["lyric_url"].value<std::string>();
                auto title = properties["title"].value<std::string>();
                auto artist = properties["artist"].value<std::string>();
                auto song_name = properties["song_name"].value<std::string>();

                if (!audio_url.empty()) {
                    auto esp32_music = static_cast<Esp32Music*>(music);
                    if (!esp32_music->PlayDirect(audio_url, lyric_url, lyric_text, title, artist)) {
                        return "{\"success\": false, \"message\": \"直链播放失败\"}";
                    }
                    return "{\"success\": true, \"message\": \"音乐开始播放\"}";
                }

                // 没给直链：设备自己按歌名联网解析
                std::string want = song_name.empty() ? title : song_name;
                if (want.empty()) {
                    return "{\"success\": false, \"message\": \"缺少 audio_url 或 song_name\"}";
                }
                if (!music->Download(want, artist)) {
                    return "{\"success\": false, \"message\": \"获取音乐资源失败\"}";
                }
                auto download_result = music->GetDownloadResult();
                ESP_LOGI(TAG, "Music details result: %s", download_result.c_str());
                return "{\"success\": true, \"message\": \"音乐开始播放\"}";
            });
 
         AddTool("self.music.play_queue",
             "整张歌单连播。songs 传 JSON 数组，设备自己按顺序一首接一首播，播完自动接下一首，中间不插话。\n"
             "参数:\n"
             "  `songs`: JSON 数组字符串，每项 {audio_url, lyric_text, title, artist}，audio_url 必填。\n"
             "返回:\n"
             "  入队与开播结果。用户说“播放整个歌单/这张歌单/接着放”就用它，不要自己一首一首下发。",
             PropertyList({
                 Property("songs", kPropertyTypeString)
             }),
             [music](const PropertyList& properties) -> ReturnValue {
                 auto songs = properties["songs"].value<std::string>();
                 auto esp32_music = static_cast<Esp32Music*>(music);
                 if (!esp32_music->PlayQueue(songs)) {
                     return "{\"success\": false, \"message\": \"歌单播放失败\"}";
                 }
                 return "{\"success\": true, \"message\": \"歌单已开始连播\"}";
             });
 

         AddTool("self.music.browse_radio",
             "用户说的是想听什么「场景/感觉」而不是具体台名时，用它找候选台"
             "（“想听点适合学习的”“开车路上听的”“睡前想安静点”“有没有怀旧的”“想听粤语”“想听新闻”）。\n"
             "只查不播。拿到清单后挑 2~3 个，用地道口语把台名和风格念给用户听，问他要哪个。\n"
             "参数:\n"
             "  `want`: 需求关键词（学习、专注、开车、睡前、怀旧、粤语、新闻、某个城市）。留空返回一批通用台。\n"
             "返回:\n"
             "  {\"count\":N,\"stations\":[{\"index\":序号,\"name\":台名,\"desc\":一句话风格}]}\n"
             "  用户定下哪个，用 self.music.play_radio 带 index 播，别自己编台名。清单为空就把需求问细一点。",
             PropertyList({
                 Property("want", kPropertyTypeString, "")
             }),
             [](const PropertyList& properties) -> ReturnValue {
                 return Esp32Music::BrowseRadio(properties["want"].value<std::string>());
             });

         AddTool("self.music.play_radio",
             "播放某个具体电台（无限直播流）。用户点名了台名、或者从候选里挑定了，用它。\n"
             "用户只是模糊表达（“放个广播”“随便听点什么”“想听适合学习的”）时先调 self.music.browse_radio，"
             "把候选念给用户挑，他选完再带 index 回来播——别替用户挑一个就播。\n"
             "参数:\n"
             "  `station`: 台名或别名（用户点名时传，如 中国之声、广东、浙江音乐、怀旧、北京新闻）\n"
             "  `index`: browse_radio 返回的序号（用户从候选里挑定时传，最稳，不会念错台名）\n"
             "  `url`: 音频流直链（用户自己给了地址才传）\n"
             "返回:\n"
             "  success=true 才是真播上了。need_choice=true 说明用户没点名：把 candidates 念成 2~3 个选择给他。\n"
             "  success=false 且带 available：台名对不上，照 available 让用户重选。",
             PropertyList({
                 Property("station", kPropertyTypeString, ""),
                 Property("index", kPropertyTypeInteger, 0, 0, 999),
                 Property("url", kPropertyTypeString, "")
             }),
             [music](const PropertyList& properties) -> ReturnValue {
                 auto station = properties["station"].value<std::string>();
                 auto url = properties["url"].value<std::string>();
                 auto index = properties["index"].value<int>();
                 if (url.empty() && index <= 0 && Esp32Music::IsGenericRadioName(station)) {
                     return "{\"success\": false, \"need_choice\": true, \"candidates\": " +
                            Esp32Music::BrowseRadio("") +
                            ", \"message\": \"用户没点名，别替他挑。从 candidates 里挑 2~3 个，连台名带风格念给他，问他要哪个。\"}";
                 }
                 auto esp32_music = static_cast<Esp32Music*>(music);
                 if (!esp32_music->PlayRadio(station, url, index)) {
                     return "{\"success\": false, \"message\": \"电台没播起来（4 秒内没拉到一个字节音频）。台名对不上就照 available 让用户重选；对得上就是网络或源不可达。要如实说没放出来，绝不能说已开播。\", \"available\": \"" + Esp32Music::RadioStationNames() + "\"}";
                 }
                 return "{\"success\": true, \"message\": \"电台已开始播放\"}";
             });

         AddTool("self.music.stop",
             "停止当前播放：歌曲、歌单、电台都停，顺带丢掉排队的下一首。\n"
             "参数: 无。\n"
             "返回:\n"
             "  停止结果。用户说“停/别放了/关掉音乐/关掉广播”就用它。",
             PropertyList(),
             [music](const PropertyList& properties) -> ReturnValue {
                 auto esp32_music = static_cast<Esp32Music*>(music);
                 esp32_music->StopStreaming();
                 return "{\"success\": true, \"message\": \"已停止播放\"}";
             });

         AddTool("self.music.set_display_mode",
             "设置音乐播放时的显示模式。可以选择显示频谱或歌词，比如用户说‘打开频谱’或者‘显示频谱’，‘打开歌词’或者‘显示歌词’就设置对应的显示模式。\n"
             "参数:\n"
             "  `mode`: 显示模式，可选值为 'spectrum'（频谱）或 'lyrics'（歌词）。\n"
             "返回:\n"
             "  设置结果信息。",
             PropertyList({
                 Property("mode", kPropertyTypeString)//显示模式: "spectrum" 或 "lyrics"
             }),
             [music](const PropertyList& properties) -> ReturnValue {
                 auto mode_str = properties["mode"].value<std::string>();
                 
                 // 转换为小写以便比较
                 std::transform(mode_str.begin(), mode_str.end(), mode_str.begin(), ::tolower);
                 
                 if (mode_str == "spectrum" || mode_str == "频谱") {
                     // 设置为频谱显示模式
                     auto esp32_music = static_cast<Esp32Music*>(music);
                     esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_SPECTRUM);
                     return "{\"success\": true, \"message\": \"已切换到频谱显示模式\"}";
                 } else if (mode_str == "lyrics" || mode_str == "歌词") {
                     // 设置为歌词显示模式
                     auto esp32_music = static_cast<Esp32Music*>(music);
                     esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_LYRICS);
                     return "{\"success\": true, \"message\": \"已切换到歌词显示模式\"}";
                 } else {
                     return "{\"success\": false, \"message\": \"无效的显示模式，请使用 'spectrum' 或 'lyrics'\"}";
                 }
                 
                 return "{\"success\": false, \"message\": \"设置显示模式失败\"}";
             });
     }
 
     // 闹钟：到点由板子自己定时并播放，服务端下线或断网也挡不住（不依赖 WS 连接）
     // 上次开机存的闹钟与铃声先补回来；时间未同步它会自己跳过，下次调用再试
     Alarm::GetInstance().Restore();

     AddTool("self.alarm.set",
          "设闹钟、定时任务或倒计时。到点设备自己响，本地定时，不依赖服务端在线。\n"
         "参数（`clock` 和 `seconds` 二选一）：\n"
         "  `clock`: 绝对时刻，格式 \"HH:MM\" 24 小时制，如 \"07:00\"。\n"
         "  `seconds`: 倒计时秒数（1-86400）。\n"
          "  `weekdays`: clock 模式下什么时候响——“workday”（工作日）/“weekend”（周末）/“daily”（每天）/“1,3,5”（周一三五，0=周日）；不传则只响一次。\n"
          "  `daily`: 老参数，等同一个星期都响；给了 weekdays 就以 weekdays 为准。\n"
           "  `label`: 到点要提醒的那件事本身，如“吃降压药”“接孩子”“开周会”——用户说“提醒我几点做什么”时必须把“做什么”原话放进来。\n"
           "  `audio_url`: 响铃播放的音频直链（有就传，最快）。\n"
           "  `song_name`/`artist`: 按歌名让设备自己搜曲播放，到点放这首歌。\n"
           "  到点一定出声：先念 label 那件事，再放铃声。没点名放什么就用默认铃声《好运来》（祖海）——别把自己的默认说成用户点的。\n"
         "返回:\n"
          "  编号与下次响铃时间。用户说“明早七点叫我”用 clock（不传 weekdays）；说“每天早上/工作日上午七点”用 clock+weekdays；说“十分钟后提醒我”用 seconds。",
         PropertyList({
             Property("clock", kPropertyTypeString, ""),
             Property("seconds", kPropertyTypeInteger, 0, 0, 86400),
              Property("daily", kPropertyTypeBoolean, false),
              Property("weekdays", kPropertyTypeString, ""),
             Property("label", kPropertyTypeString, ""),
             Property("audio_url", kPropertyTypeString, ""),
             Property("song_name", kPropertyTypeString, ""),
             Property("artist", kPropertyTypeString, "")
         }),
         [](const PropertyList& properties) -> ReturnValue {
             Alarm::GetInstance().Restore();
             auto clock = properties["clock"].value<std::string>();
             auto seconds = properties["seconds"].value<int>();
             auto daily = properties["daily"].value<bool>();
             auto audio_url = properties["audio_url"].value<std::string>();
             auto song_name = properties["song_name"].value<std::string>();
             auto artist = properties["artist"].value<std::string>();
              auto label = properties["label"].value<std::string>();

              int mask = ParseWeekdays(properties["weekdays"].value<std::string>());
              if (mask < 0) {
                  return std::string(
                      "{\"success\": false, \"message\": \"weekdays 只认 daily/workday/weekend 或 0-6 的逗号列表（0=周日，1=周一）\"}");
              }
              if (daily) {
                  mask = 0x7F;
              }

              if (!clock.empty()) {
                  int hour = -1, minute = -1;
                  if (sscanf(clock.c_str(), "%d:%d", &hour, &minute) != 2) {
                      return std::string("{\"success\": false, \"message\": \"clock 格式要 HH:MM\"}");
                  }
                  return Alarm::GetInstance().SetClock(hour, minute, mask, audio_url, song_name, artist,
                                                       label);
              }
             if (seconds > 0) {
                 return Alarm::GetInstance().SetTimer(seconds, audio_url, song_name, artist, label);
             }
             return std::string("{\"success\": false, \"message\": \"clock 和 seconds 都没给\"}");
         });

     AddTool("self.alarm.list",
         "列出所有闹钟，并带上次到点的提醒 last_fired（哪件事、几点到点）。用户问“我设了哪些闹钟”，或者提到刚才响过的提醒/闹钟时用它——先看 last_fired 再说话，别猜。",
         PropertyList(),
         [](const PropertyList& properties) -> ReturnValue {
             return std::string("{\"last_fired\": ") + Alarm::GetInstance().LastFiredJson() +
                    ", \"alarms\": " + Alarm::GetInstance().List() + "}";
         });

     AddTool("self.alarm.cancel",
         "取消闹钟。`id` 传编号只取消那一个；不传或传 0 取消全部。",
         PropertyList({
             Property("id", kPropertyTypeInteger, 0, 0, 9999)
         }),
         [](const PropertyList& properties) -> ReturnValue {
             return Alarm::GetInstance().Cancel(properties["id"].value<int>());
         });

     AddTool("self.alarm.ring",
         "改闹钟响铃放什么，响铃时间不变（不用取消重建）。\n"
         "  `id`: 闹钟编号，传了就只改那一个；不传或传 0 改的是**默认铃声**，"
         "以后新建的闹钟都放它。\n"
         "  `song_name`/`artist`: 换成的歌，设备自己搜曲播放。\n"
         "  `audio_url`: 有直链就传，比按歌名搜更快更准。\n"
         "用户说“闹钟铃声换成《稻香》”“以后默认用晴天叫我”都用它。",
         PropertyList({
             Property("id", kPropertyTypeInteger, 0, 0, 9999),
             Property("song_name", kPropertyTypeString, ""),
             Property("artist", kPropertyTypeString, ""),
             Property("audio_url", kPropertyTypeString, "")
         }),
         [](const PropertyList& properties) -> ReturnValue {
             return Alarm::GetInstance().SetRing(
                 properties["id"].value<int>(),
                 properties["audio_url"].value<std::string>(),
                 properties["song_name"].value<std::string>(),
                 properties["artist"].value<std::string>());
         });

     // 待机屏那行天气：数据由固件自己拉，AI 只负责换城市和读现成的
     AddTool("self.weather.set_city",
         "换待机屏天气显示的城市。用户说“天气改成杭州”“我在上海”时用它。\n"
         "  `city`: 中文地名，如“杭州”“承德”，只写地名不要带省。\n"
         "返回 success 时 now 就是换好之后屏幕上的那行字，照着说就行；失败说明地名没查到。",
         PropertyList({
             Property("city", kPropertyTypeString, "")
         }),
         [](const PropertyList& properties) -> ReturnValue {
             return Weather::GetInstance().SetCity(properties["city"].value<std::string>());
         });

     AddTool("self.weather.get",
         "读待机屏现在显示的天气。用户问“今天天气怎么样”“外面冷不冷”时用它，别自己编。",
         PropertyList(),
         [](const PropertyList&) -> ReturnValue {
             auto text = Weather::GetInstance().Text();
             if (text.empty()) {
                 return std::string("{\"success\": false, \"message\": \"天气还没拉到（可能没联网）\"}");
             }
             return std::string("{\"success\": true, \"now\": \"") + text + "\"}";
         });

     // Restore the original tools list to the end of the tools list
     tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
 }
 
 void McpServer::AddTool(McpTool* tool) {
     // Prevent adding duplicate tools
     if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
         ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
         return;
     }
 
     ESP_LOGI(TAG, "Add tool: %s", tool->name().c_str());
     tools_.push_back(tool);
 }
 
 void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
     AddTool(new McpTool(name, description, properties, callback));
 }
 
 void McpServer::ParseMessage(const std::string& message) {
     cJSON* json = cJSON_Parse(message.c_str());
     if (json == nullptr) {
         ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
         return;
     }
     ParseMessage(json);
     cJSON_Delete(json);
 }
 
 void McpServer::ParseCapabilities(const cJSON* capabilities) {
     auto vision = cJSON_GetObjectItem(capabilities, "vision");
     if (cJSON_IsObject(vision)) {
         auto url = cJSON_GetObjectItem(vision, "url");
         auto token = cJSON_GetObjectItem(vision, "token");
         if (cJSON_IsString(url)) {
             auto camera = Board::GetInstance().GetCamera();
             if (camera) {
                 std::string url_str = std::string(url->valuestring);
                 std::string token_str;
                 if (cJSON_IsString(token)) {
                     token_str = std::string(token->valuestring);
                 }
                 camera->SetExplainUrl(url_str, token_str);
             }
         }
     }
 }
 
 void McpServer::ParseMessage(const cJSON* json) {
     // Check JSONRPC version
     auto version = cJSON_GetObjectItem(json, "jsonrpc");
     if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
         ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
         return;
     }
     
     // Check method
     auto method = cJSON_GetObjectItem(json, "method");
     if (method == nullptr || !cJSON_IsString(method)) {
         ESP_LOGE(TAG, "Missing method");
         return;
     }
     
     auto method_str = std::string(method->valuestring);
     if (method_str.find("notifications") == 0) {
         return;
     }
     
     // Check params
     auto params = cJSON_GetObjectItem(json, "params");
     if (params != nullptr && !cJSON_IsObject(params)) {
         ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
         return;
     }
 
     auto id = cJSON_GetObjectItem(json, "id");
     if (id == nullptr || !cJSON_IsNumber(id)) {
         ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
         return;
     }
     auto id_int = id->valueint;
     
     if (method_str == "initialize") {
         if (cJSON_IsObject(params)) {
             auto capabilities = cJSON_GetObjectItem(params, "capabilities");
             if (cJSON_IsObject(capabilities)) {
                 ParseCapabilities(capabilities);
             }
         }
         auto app_desc = esp_app_get_description();
         std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
         message += app_desc->version;
         message += "\"}}";
         ReplyResult(id_int, message);
     } else if (method_str == "tools/list") {
         std::string cursor_str = "";
         if (params != nullptr) {
             auto cursor = cJSON_GetObjectItem(params, "cursor");
             if (cJSON_IsString(cursor)) {
                 cursor_str = std::string(cursor->valuestring);
             }
         }
         GetToolsList(id_int, cursor_str);
     } else if (method_str == "tools/call") {
         if (!cJSON_IsObject(params)) {
             ESP_LOGE(TAG, "tools/call: Missing params");
             ReplyError(id_int, "Missing params");
             return;
         }
         auto tool_name = cJSON_GetObjectItem(params, "name");
         if (!cJSON_IsString(tool_name)) {
             ESP_LOGE(TAG, "tools/call: Missing name");
             ReplyError(id_int, "Missing name");
             return;
         }
         auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
         if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
             ESP_LOGE(TAG, "tools/call: Invalid arguments");
             ReplyError(id_int, "Invalid arguments");
             return;
         }
         auto stack_size = cJSON_GetObjectItem(params, "stackSize");
         if (stack_size != nullptr && !cJSON_IsNumber(stack_size)) {
             ESP_LOGE(TAG, "tools/call: Invalid stackSize");
             ReplyError(id_int, "Invalid stackSize");
             return;
         }
         DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments, stack_size ? stack_size->valueint : DEFAULT_TOOLCALL_STACK_SIZE);
     } else {
         ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
         ReplyError(id_int, "Method not implemented: " + method_str);
     }
 }
 
 void McpServer::ReplyResult(int id, const std::string& result) {
     std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
     payload += std::to_string(id) + ",\"result\":";
     payload += result;
     payload += "}";
     Application::GetInstance().SendMcpMessage(payload);
 }
 
 void McpServer::ReplyError(int id, const std::string& message) {
     std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
     payload += std::to_string(id);
     payload += ",\"error\":{\"message\":\"";
     payload += message;
     payload += "\"}}";
     Application::GetInstance().SendMcpMessage(payload);
 }
 
 void McpServer::GetToolsList(int id, const std::string& cursor) {
     const int max_payload_size = 8000;
     std::string json = "{\"tools\":[";
     
     bool found_cursor = cursor.empty();
     auto it = tools_.begin();
     std::string next_cursor = "";
     
     while (it != tools_.end()) {
         // 如果我们还没有找到起始位置，继续搜索
         if (!found_cursor) {
             if ((*it)->name() == cursor) {
                 found_cursor = true;
             } else {
                 ++it;
                 continue;
             }
         }
         
         // 添加tool前检查大小
         std::string tool_json = (*it)->to_json() + ",";
         if (json.length() + tool_json.length() + 30 > max_payload_size) {
             // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
             next_cursor = (*it)->name();
             break;
         }
         
         json += tool_json;
         ++it;
     }
     
     if (json.back() == ',') {
         json.pop_back();
     }
     
     if (json.back() == '[' && !tools_.empty()) {
         // 如果没有添加任何tool，返回错误
         ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
         ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
         return;
     }
 
    // 服务器端只会看到这一页里的工具，分页丢了什么必须能从串口看出来
    ESP_LOGI(TAG, "tools/list: %d tools registered, page payload=%u bytes, next=%s",
             (int)tools_.size(), (unsigned)json.size(),
             next_cursor.empty() ? "none" : next_cursor.c_str());

    if (next_cursor.empty()) {
        json += "]}";
    } else {
         json += "],\"nextCursor\":\"" + next_cursor + "\"}";
     }
     
     ReplyResult(id, json);
 }
 
 void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments, int stack_size) {
     auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                  [&tool_name](const McpTool* tool) { 
                                      return tool->name() == tool_name; 
                                  });
     
     if (tool_iter == tools_.end()) {
         ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
         ReplyError(id, "Unknown tool: " + tool_name);
         return;
     }
 
     PropertyList arguments = (*tool_iter)->properties();
     try {
         for (auto& argument : arguments) {
             bool found = false;
             if (cJSON_IsObject(tool_arguments)) {
                 auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                 if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                     argument.set_value<bool>(value->valueint == 1);
                     found = true;
                 } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                     argument.set_value<int>(value->valueint);
                     found = true;
                 } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                     argument.set_value<std::string>(value->valuestring);
                     found = true;
                 }
             }
 
             if (!argument.has_default_value() && !found) {
                 ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                 ReplyError(id, "Missing valid argument: " + argument.name());
                 return;
             }
         }
     } catch (const std::exception& e) {
         ESP_LOGE(TAG, "tools/call: %s", e.what());
         ReplyError(id, e.what());
         return;
     }
 
    // AI 说「设好了」但其实没调工具，是这套东西最难查的故障：工具调用是唯一
    // 能证伪的入口，参数和返回都在这里留痕。
    if (cJSON_IsObject(tool_arguments)) {
        char* dump = cJSON_PrintUnformatted(tool_arguments);
        ESP_LOGI(TAG, "tools/call %s args: %.220s", tool_name.c_str(), dump ? dump : "{}");
        if (dump) {
            cJSON_free(dump);
        }
    } else {
        ESP_LOGI(TAG, "tools/call %s args: (none)", tool_name.c_str());
    }

    // 播放音频时内部 RAM 会掉到 1.7KB，6KB 内部栈建线程必然失败；
    // 而 std::thread 失败是抛 system_error，没人接就 terminate→abort 重启。
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "tool_call";
    cfg.stack_size = stack_size;
    cfg.prio = 1;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= 64 * 1024) {
        cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
    esp_pthread_set_cfg(&cfg);
    // cfg 是线程局部的，会污染本线程后面建的所有线程，用完立刻还原
    esp_pthread_cfg_t restore = esp_pthread_get_default_config();

    PropertyList fallback_args = arguments;
    bool started = false;
    try {
        // Use a thread to call the tool to avoid blocking the main thread
        tool_call_thread_ = std::thread([this, id, tool_iter, tool_name, arguments = std::move(arguments)]() {
            try {
                std::string result = (*tool_iter)->Call(arguments);
                ESP_LOGI(TAG, "tools/call %s -> %.220s", tool_name.c_str(), result.c_str());
                ReplyResult(id, result);
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "tools/call: %s", e.what());
                ReplyError(id, e.what());
            }
        });
        tool_call_thread_.detach();
        started = true;
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tool_call thread failed (%s), running synchronously", e.what());
    }
    esp_pthread_set_cfg(&restore);

    // 建不起线程就同步执行：这个线程只是为了不阻塞 MQTT，慢一点也远好过重启
    if (!started) {
        try {
            std::string result = (*tool_iter)->Call(fallback_args);
            ESP_LOGI(TAG, "tools/call %s -> %.220s", tool_name.c_str(), result.c_str());
            ReplyResult(id, result);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    }
 }
