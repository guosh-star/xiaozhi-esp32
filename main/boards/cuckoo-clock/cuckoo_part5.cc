// ===== Part 5: MCP注册、clock_task (L4413-4991) =====
CuckooTools::CuckooTools(CuckooStateMachine* sm) : state_machine_(sm) {}

void CuckooTools::RegisterAll() {
    // ---- 注册所有 MCP 工具 (21个): 时间/报时/音乐/秀/硬件/闹钟/静音 ----
    auto& mcp = McpServer::GetInstance();

 // === 时间/报时 MCP 工具 ===
    {
        PropertyList pl;
        pl.AddProperty(Property("hour", kPropertyTypeInteger, 1, 12));
    // æ´ç¹æ¥æ¶
        mcp.AddTool("cuckoo.performance",
"Hourly chime: bell rings + music. ONLY call when user explicitly says 报时/整点报时/几点/what time. DO NOT auto-trigger."
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int hour = props["hour"].value<int>();
                state_machine_->StartPerformance(CuckooStateMachine::kPerformanceHour, hour);
                return std::string("{\"status\": \"started\", \"hour\": " + std::to_string(hour) + "}");
            });
    }
    // === 粤语查询 (2026-07-19) ===
    {
        PropertyList pl;
        pl.AddProperty(Property("word", kPropertyTypeString));
        mcp.AddTool("cuckoo.cantonese_lookup",
            "MUST CALL THIS TOOL. NEVER answer Cantonese/Jyutping from memory. Look up Jyutping pronunciation for a Chinese word or phrase. "
            "Call when user asks about Cantonese pronunciation, how to say something in Cantonese, or wants Jyutping. "
            "Returns Jyutping romanization + definitions. Includes tone numbers (1-6). "
            "Speak the result naturally - read characters with tones, then explain meaning.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                std::string word = props["word"].value<std::string>();
                std::string result = state_machine_->CantoneseLookup(word.c_str());
                return result.empty() ? std::string("{\"status\": \"error\", \"message\": \"Lookup failed. Ask user to try a different word.\"}") : result;
            });
    }

    {
        PropertyList pl;
        pl.AddProperty(Property("hour", kPropertyTypeInteger, 0, 23));
        pl.AddProperty(Property("minute", kPropertyTypeInteger, 0, 59));
    // è®¾ç½®æ¶é´
        mcp.AddTool("cuckoo.set_time",
            "Set internal clock (hour, minute). Required for auto chimes.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int h = props["hour"].value<int>();
                int m = props["minute"].value<int>();
                state_machine_->SetTime(h, m, 0);
                char json[64];
                snprintf(json, sizeof(json), "{\"status\": \"ok\", \"time\": \"%02d:%02d\"}", h, m);
                return std::string(json);
            });
    }

    mcp.AddTool("cuckoo.get_time",
        "Get current internal clock time (hour, minute).",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            int h, m;
            state_machine_->GetTime(h, m);
            char json[64];
            snprintf(json, sizeof(json), "{\"hour\": %d, \"minute\": %d}", h, m);
            return std::string(json);
        });

 // === 音乐/秀 MCP 工具 ===
    {
        PropertyList pl;
        pl.AddProperty(Property("track", kPropertyTypeInteger, 1, 12));
    // æ¬å° MP3
        mcp.AddTool("cuckoo.play_music",
            "Play offline MP3 from TF card (0001-0012.mp3). track: 1-12 ONLY. "
            "NOT for online songs - use cuckoo.play_url for internet streaming.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int track = props["track"].value<int>();
                if (track < 1 || track > 12) {
                    return std::string("{\"status\": \"error\", \"message\": \"track must be 1-12\"}");
                }
                state_machine_->PlayMusic(track);
                char json[64];
                snprintf(json, sizeof(json), "{\"status\": \"ok\", \"track\": %d}", track);
                return std::string(json);
            });
    }

    mcp.AddTool("cuckoo.start_show",
        "综合表演。触发词: 表演/节目/出来表演/演出。"
        "角色工具优先: 花园→cuckoo.garden_show, 琳达→cuckoo.linda_show, 小狗→cuckoo.dog_show。"
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StartShow();
            return std::string("{\"status\": \"show_started\"}");
        });

    // åæ­¢ææ
    mcp.AddTool("cuckoo.stop_all",
        "Stop motors, chime, performance. Does NOT stop music - use cuckoo.stop_music to stop music.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAll();
            return std::string("{\"status\": \"stopped\"}");
        });

    // åæ­¢é³ä¹
    mcp.AddTool("cuckoo.stop_music",
"Stop music playback. Call ONLY when user explicitly asks to stop the music. Do NOT call for performance/waking."
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopMusic();
            return std::string("{\"status\": \"music_stopped\"}");
        });

    // === 小朋友们控制 ===
    mcp.AddTool("cuckoo.kids_come_out",
        "让小朋友们出来一起欣赏音乐：小狗舵机、舞蹈电机、吉他舵机随音乐节奏摆动。",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->KidsComeOut();
            return std::string("{\"status\": \"kids_come_out\"}");
        });

    mcp.AddTool("cuckoo.kids_rest",
        "让小朋友们回去休息：停止所有动作（小狗舵机、舞蹈电机、吉他舵机不动），只保留音乐播放。",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->KidsRest();
            return std::string("{\"status\": \"kids_rest\"}");
        });

 // === 硬件控制 MCP 工具 ===
    // èè¹
    mcp.AddTool("cuckoo.dance",
        "Dance routine: M1+M2 motors + violin servo.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->Dance();
            return std::string("{\"status\": \"dancing\"}");
        });


    // æå¼é¨
    mcp.AddTool("cuckoo.open_door",
        "Open bird door motor.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->OpenDoor();
            return std::string("{\"status\": \"door opened\"}");
        });

    mcp.AddTool("cuckoo.close_door",
        "Close bird door motor.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->CloseDoor();
            return std::string("{\"status\": \"door closed\"}");
        });




    // === 闹钟 MCP 工具 ===
    {
        PropertyList pl;
        pl.AddProperty(Property("hour", kPropertyTypeInteger, 0, 23));
        pl.AddProperty(Property("minute", kPropertyTypeInteger, 0, 59));
        pl.AddProperty(Property("repeat_daily", kPropertyTypeInteger, 0, 1));
    // è®¾ç½®é¹é
        mcp.AddTool("cuckoo.set_alarm",
            "Set alarm at hour:minute. Ask user if repeat_daily first (1=daily, 0=once). Max 5 alarms.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int h = props["hour"].value<int>();
                int m = props["minute"].value<int>();
                bool repeat = props["repeat_daily"].value<int>() != 0;
                state_machine_->SetAlarm(h, m, repeat);
                char json[128];
                snprintf(json, sizeof(json),
                    "{\"status\": \"ok\", \"time\": \"%02d:%02d\", \"repeat_daily\": %s}",
                    h, m, repeat ? "true" : "false");
                return std::string(json);
            });
    }

    mcp.AddTool("cuckoo.get_alarms",
        "List all alarms (index, hour, minute, repeat). Returns JSON array.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            return state_machine_->GetAlarmsJson();
        });

    {
        PropertyList pl;
        pl.AddProperty(Property("index", kPropertyTypeInteger, 1, 5));
        mcp.AddTool("cuckoo.delete_alarm",
            "Delete alarm by index from get_alarms.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int idx = props["index"].value<int>();
                bool ok = state_machine_->DeleteAlarm(idx);
                return std::string(ok ? "{\"status\": \"deleted\"}"
                                      : "{\"status\": \"error\", \"message\": \"invalid index\"}");
            });
    }

    mcp.AddTool("cuckoo.stop_alarm",
"Stop a ringing ALARM only. For 闹铃/停铃. NOT for stopping music/performance."
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            state_machine_->StopAlarm();
            return std::string("{\"status\": \"alarm_stopped\"}");
        });

    // === 静音模式 MCP 工具 ===
    {
        PropertyList pl;
        pl.AddProperty(Property("mode", kPropertyTypeInteger, 0, 3));
        pl.AddProperty(Property("start_hour", kPropertyTypeInteger, 0, 23));
        pl.AddProperty(Property("end_hour", kPropertyTypeInteger, 0, 23));
    // éé³æ¨¡å¼
        mcp.AddTool("cuckoo.set_quiet_mode",
"Set chime quiet mode. 0=全天静音(永不报时), 1=全天报时, 2=光线静音(LDR光照), 3=指定时间段静音(start_hour~end_hour间静音)."
            "Default start_hour=22 end_hour=6.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                int m = props["mode"].value<int>();
                int sh = props["start_hour"].value<int>();
                int eh = props["end_hour"].value<int>();
                state_machine_->quiet_mode_ = m;
                state_machine_->quiet_start_ = sh;
                state_machine_->quiet_end_ = eh;
                state_machine_->SaveQuietMode();
                char json[128];
                snprintf(json, sizeof(json),
                    "{\"status\": \"ok\", \"mode\": %d, \"start_hour\": %d, \"end_hour\": %d}", m, sh, eh);
                return std::string(json);
            });
    }

    mcp.AddTool("cuckoo.get_quiet_mode",
        "Get current chime quiet mode: mode(0~3), start_hour, end_hour. "
        "Return JSON, AI must translate to user-friendly description.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            int m = state_machine_->quiet_mode_.load();
            int sh = state_machine_->quiet_start_.load();
            int eh = state_machine_->quiet_end_.load();
            char json[192];
            snprintf(json, sizeof(json),
                "{\"mode\": %d, \"start_hour\": %d, \"end_hour\": %d, "
                "\"desc\": \"mode=%d: %s\"}",
                m, sh, eh, m,
m == 0 ? "全天静音" : m == 1 ? "全天报时" :
m ==m == 2 ? "光线静音(LDR)" : "时间段静音");
            return std::string(json);
        });

    // === 状态查询 ===
    // ç¶ææ¥è¯¢
    mcp.AddTool("cuckoo.get_status",
        "查询设备状态：running（是否正在表演），music_playing（音乐是否正在播放）。\n"
        "【强制规则】回复用户任何关于演出/表演/音乐/歌舞的问题前，必须先调用本工具。不得凭记忆或上下文猜测状态。\n"
        "如果 running 是 false 但用户提到演出，直接说演出已经结束，不要说还在进行。",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            auto& audio = Application::GetInstance().GetAudioService();
            bool music_on = audio.IsBgAudioActive();
            char json[192];
            snprintf(json, sizeof(json),
                "{\"running\": %s, \"music_playing\": %s}",
                state_machine_->IsRunning() ? "true" : "false",
                music_on ? "true" : "false");
            return std::string(json);
        });

 // === 在线音乐 ===
 // 音乐代理在启动时自动配置（config.h 里的 DEFAULT_MUSIC_PROXY_HOST/PORT）。
 // set_music_proxy 是空操作占位工具，永远瞬间成功，不做TCP连接检查。
 // 这个工具只是为了满足 AI 在调用 play_url 之前先调 set_music_proxy 的习惯。

    {
        PropertyList pl;
        pl.AddProperty(Property("url", kPropertyTypeString));
    // å¨çº¿é³ä¹
        mcp.AddTool("cuckoo.play_url",
            "播放在线音乐。用户说出歌名后立即调用本工具，不要先问用户问题。\n""参数url格式：'/pcm?q=歌名'（中文需要URL编码，空格用%%20）。\n""【最重要规则】只传歌名本身，绝不要自作主张加歌手名！系统会自动检测多版本并询问用户。\n""如果正在放歌，用户要换歌，直接传新歌名即可，系统自动停旧播新。\n""不要问用户'要不要换'——直接调用工具。\n""不要光说'我来放歌'——必须实际调用play_url。\n""示例：用户说'放酒干倘卖无'→调用play_url(\'/pcm?q=%E9%85%92%E5%B9%B2%E5%80%98%E5%8D%96%E6%97%A0\')。\n""本地歌曲请用cuckoo.play_music（曲目1-12），不要用play_url。",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                std::string url = props["url"].value<std::string>();
                // 先检查多版本：如果服务器返回歌手列表JSON，直接返回给AI念（不播歌）
                std::string multi = state_machine_->CheckMultiArtist(url.c_str());
                if (!multi.empty()) {
                return multi;  // 歌手列表JSON返回给AI，AI自动念出来让用户选
                }
                int ret = state_machine_->PlayOnlineMusic(url.c_str());
                if (ret >= 0) {
                    return std::string("{\"status\": \"ok\", \"playing\": true}");
                } else {
                    char json[160];
                    snprintf(json, sizeof(json),
                        "{\"status\": \"error\", \"code\": %d, \"hint\": \"Playback failed. You may retry.\"}", ret);
                    return std::string(json);
                }
            });
    }
    // å°çç§

    mcp.AddTool("cuckoo.dog_show",
        "小狗秀。触发词: 小狗/狗狗/丽莎/丽莎来一个。"

        "Dog show: call when user asks about dog/puppy/Lisa. Keep response very brief - one short sentence only.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->DogShow();
            return std::string("{\"status\": \"dog_show_started\"}");
        });

    mcp.AddTool("cuckoo.linda_show",
        "小狗秀。触发词: 小狗/狗狗/丽莎/丽莎来一个。"

        "Linda show: call when user asks about Linda or dancing.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->StartLindaShow();
            return std::string("{\"status\": \"linda_show_started\"}");
        });

    // è±å­ç§
    mcp.AddTool("cuckoo.garden_show",
        "琳达秀。触发词: 琳达/应援/跳舞。"

"Gar"花园秀。触发词: 花园/小提琴。"
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_machine_->IsRunning()) return std::string("{\"status\": \"busy\", \"message\": \"Another show is still running. Tell the user to wait for it to finish.\"}");
            state_machine_->StartGardenShow();
            return std::string("{\"status\": \"garden_show_started\"}");
        });

    // === 整点表演开关 ===
    {
        PropertyList pl;
        pl.AddProperty(Property("enabled", kPropertyTypeBoolean, true));
        mcp.AddTool("cuckoo.set_hourly_performance",
            "Enable/disable full performance after hourly bell. Default on. When disabled bell only.",
            pl,
            [this](const PropertyList& props) -> ReturnValue {
                bool en = props["enabled"].value<bool>();
                state_machine_->hourly_perf_.store(en);
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"performance\":%s}", en ? "enabled" : "disabled");
                return std::string(buf);
            });
    }
    
    mcp.AddTool("cuckoo.get_hourly_performance",
        "Check if hourly chime performance is currently enabled.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            bool en = state_machine_->hourly_perf_.load();
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"performance\":%s}", en ? "enabled" : "disabled");
            return std::string(buf);
        });

}

// ============================================

//
// 时钟任务 (250ms tick)

// 从 NTP 获取当前时间






// ============================================
// 250mså®æ¶å¾ªç¯ (Core 1)
void cuckoo_clock_task(void* params) {

    ESP_LOGI(TAG, "Reset reason: cpu0=%d cpu1=%d",
 * 时间同步 + 整点/半点检查 + 黑暗检测 + 闹钟触发 + 音乐舞蹈 tick
             esp_reset_reason(), esp_reset_reason());
    // è¯»åä¸æ¬¡å´©æºæ¥å¿
    if (rtc_crash_log.magic == 0xCAFEBABE && rtc_crash_log.tick_sec > 0) {
        ESP_LOGW(TAG, "Last heartbeat before crash/reset: t=%ds state=%d music=%d heap=%d",
                 (int)rtc_crash_log.tick_sec, (int)rtc_crash_log.dev_state,
                 (int)rtc_crash_log.music_active, (int)rtc_crash_log.free_heap);
    }
    rtc_crash_log.magic = 0xCAFEBABE;

    ESP_LOGI(TAG, "Cuckoo clock task started on core %d, stack=%d",
             xPortGetCoreID(), (int)uxTaskGetStackHighWaterMark(NULL));

    auto* sm = static_cast<CuckooStateMachine*>(params);


    auto& as = Application::GetInstance().GetAudioService();
    as.RefreshOutputTimestamp();
    as.RefreshInputTimestamp();

// 从 NTP 获取当前时间

// 从 NTP 获取当前时间
    {
            // ---- NTP 时间同步 ----
        struct timeval tv;
            // 获取系统时间 (已通过NTP同步)
    // ---- NTP ----
        gettimeofday(&tv, nullptr);
            // 2001年之后 = 已同步NTP，可进行正常报时
        if (tv.tv_sec > 1000000000) {
            struct tm timeinfo;
            localtime_r(&tv.tv_sec, &timeinfo);
            sm->current_hour_ = timeinfo.tm_hour;
            sm->current_min_ = timeinfo.tm_min;
            sm->current_sec_ = timeinfo.tm_sec;
            sm->time_set_ = true;
            ESP_LOGI(TAG, "Auto-synced clock from NTP: %02d:%02d:%02d",
                     sm->current_hour_.load(), sm->current_min_.load(), sm->current_sec_.load());
        } else {
            ESP_LOGW(TAG, "NTP not synced yet, internal clock waiting for cuckoo.set_time");
        }
    }


    sm->SetMusicProxy(DEFAULT_MUSIC_PROXY_HOST, DEFAULT_MUSIC_PROXY_PORT);


    sm->LoadAlarmsFromNvs();
    sm->LoadQuietMode();
    sm->LoadKidsActive();

uint32_t tick_sec = 0;



// 时钟任务 (250ms tick)

    uint32_t sub_tick = 0;
    while (1) {
            // 250ms 定时循环 —— 主时钟心跳
        vTaskDelay(pdMS_TO_TICKS(250));
        sub_tick++;


        {
            auto dev_state = (int)Application::GetInstance().GetDeviceState();
            if (sm->prev_device_state_ == (int)kDeviceStateIdle && dev_state != (int)kDeviceStateIdle) {
                sm->last_idle_exit_us_ = esp_timer_get_time();
                    // 设备唤醒 → 开门
            ESP_LOGI(TAG, "Device woke up - opening bird door");
                sm->OpenBirdDoor();


                if (!sm->IsRunning())
                    Application::GetInstance().GetAudioService().SetWakeWordThreshold(0.3f);


                Application::GetInstance().GetAudioService().SetInputGain(30.0f);
            } else if (sm->prev_device_state_ != (int)kDeviceStateIdle && dev_state == (int)kDeviceStateIdle) {
                    // 设备休眠 → 关门
            ESP_LOGI(TAG, "Device sleeping - closing bird door");
                sm->CloseBirdDoor();


                // NOTE(2026-07-19): éå¼æ¢å¤å·²ç§»è³ä¸é¢çå®å¨åæ£æ¥


                Application::GetInstance().GetAudioService().SetInputGain(37.5f);
            }
            sm->prev_device_state_ = dev_state;

                // å®å¨å (2026-07-19): å½è®¾å¤å¤äºå®é idle æ¶
                // (æ ç§æ¼/é³ä¹)ï¼å¼ºå¶æ¢å¤é«ææåº¦å¤ééå¼ 0.02
                // ä¿®å¤æ³æ¼ 0.30 çè·¯å¾: é³ä¹æé´ä¼è¯ç»æ/ç§ idle
                // æ å¿ä½ç¡®ä¿æ¯æ¬¡å®é-idleå¥å£åªè®¾ç½®ä¸æ¬¡ (é²æ¥å¿æ·¹æ²¡)
            static bool idle_thresh_applied = false;
            bool idle_quiet = (dev_state == (int)kDeviceStateIdle) && !sm->IsRunning() &&
                              !Application::GetInstance().GetAudioService().IsBgAudioActive();
            if (idle_quiet) {
                if (!idle_thresh_applied) {
                    Application::GetInstance().GetAudioService().SetWakeWordThreshold(0.02f);
                    idle_thresh_applied = true;
                }
            } else {
                idle_thresh_applied = false;
            }


            if (dev_state == (int)kDeviceStateSpeaking) {

                int64_t ms_since_output = Application::GetInstance().GetAudioService().MsSinceLastOutput();
                if (ms_since_output < 300) {

                    sm->BirdJumpShort();
                }

            }
                 // 在线音乐播放期间的舞蹈tick (狗/小提琴)
    // èè¹ tick
             sm->MusicDanceTick();
        }

// 时钟任务 (250ms tick)
        if (sub_tick % 4 == 0) {
            tick_sec = sub_tick / 4;

// 从 NTP 获取当前时间
        if (!sm->time_set_ ? (tick_sec % 60 == 0) : (tick_sec % 300 == 0)) {
                // ---- NTP 时间同步 ----
            struct timeval tv;
                // 获取系统时间 (已通过NTP同步)
    // ---- NTP ----
            gettimeofday(&tv, nullptr);
                // 2001年之后 = 已同步NTP，可进行正常报时
            if (tv.tv_sec > 1000000000) {
                struct tm timeinfo;
                localtime_r(&tv.tv_sec, &timeinfo);
                sm->current_hour_ = timeinfo.tm_hour;
                sm->current_min_ = timeinfo.tm_min;
                sm->current_sec_ = timeinfo.tm_sec;
                sm->time_set_ = true;
                ESP_LOGI(TAG, "NTP sync: %02d:%02d:%02d",
                         sm->current_hour_.load(), sm->current_min_.load(), sm->current_sec_.load());
            } else {
                ESP_LOGW(TAG, "NTP not synced yet, will retry in 60s");
            }
        }





        if (tick_sec % 30 == 0) {
            auto& app = Application::GetInstance();
            rtc_crash_log.tick_sec = tick_sec;
            rtc_crash_log.dev_state = (uint8_t)app.GetDeviceState();
            rtc_crash_log.music_active = (uint8_t)app.GetAudioService().IsBgAudioActive();
            rtc_crash_log.free_heap = esp_get_free_heap_size();
        }


        if (tick_sec % 60 == 0) {
            ESP_LOGI(TAG, "Heartbeat t=%ds stack_hwm=%d free_heap=%d",
                     (int)tick_sec, (int)uxTaskGetStackHighWaterMark(NULL),
                     (int)esp_get_free_heap_size());
        }


        if (tick_sec % 10 == 0) {
            auto& as = Application::GetInstance().GetAudioService();
            as.RefreshOutputTimestamp();
            as.RefreshInputTimestamp();

            sm->is_dark_ = sm->CheckDark();
        }

// MCP 工具注册 (RegisterAll)
        if (sm->time_set_) {
    // ---- NTP 时间同步 ----
// 从 NTP 获取当前时间
            struct timeval tv;
                // 获取系统时间 (已通过NTP同步)
    // ---- NTP ----
            gettimeofday(&tv, nullptr);
                // 2001年之后 = 已同步NTP，可进行正常报时
            if (tv.tv_sec > 1000000000) {
                struct tm timeinfo;
                localtime_r(&tv.tv_sec, &timeinfo);
                sm->current_hour_ = timeinfo.tm_hour;
                sm->current_min_ = timeinfo.tm_min;
                sm->current_sec_ = timeinfo.tm_sec;
            } else {
// MCP 工具注册 (RegisterAll)
                sm->current_sec_++;
                if (sm->current_sec_ >= 60) {
                    sm->current_sec_ = 0;
                    sm->current_min_++;
                    if (sm->current_min_ >= 60) {
                        sm->current_min_ = 0;
                        sm->current_hour_ = (sm->current_hour_ + 1) % 24;
                    }
                }
            }


            int h = sm->current_hour_;
            int m = sm->current_min_;


            if (m == 0 && sm->current_sec_ == 0 && sm->NeedHourlyChime(h)) {
                ESP_LOGI(TAG, "Hourly chime trigger: %02d:00", h);
                    // 检查整点/半点报时触发
    // æ´ç¹/åç¹
                sm->CheckTime(h, m, sm->is_dark_);
            }

            else if (m == 30 && sm->current_sec_ == 0 && sm->NeedHalfHourlyChime(h)) {
                ESP_LOGI(TAG, "Half-hour chime trigger: %02d:30", h);
                    // 检查整点/半点报时触发
    // æ´ç¹/åç¹
                sm->CheckTime(h, m, sm->is_dark_);
            }


                // 检查闹钟触发
    // é¹é

            sm->CheckAlarms(h, m, sm->current_sec_);
        }
        }  // sub_tick >= 4 guard
// 时钟任务 (250ms tick)
    }
}
