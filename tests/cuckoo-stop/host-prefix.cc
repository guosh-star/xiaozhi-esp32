#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
#define portTICK_PERIOD_MS 1
#define CUCKOO_STOP_HOST 1
using TickType_t=uint32_t; using BaseType_t=int;using UBaseType_t=unsigned;using TaskHandle_t=void*;using gpio_num_t=int;
constexpr int pdPASS=1,pdFAIL=0,tskNO_AFFINITY=-1;
thread_local uintptr_t task_id=1;
TaskHandle_t xTaskGetCurrentTaskHandle(){return reinterpret_cast<void*>(task_id);}
void vTaskDelay(TickType_t);
void vTaskDelete(void*){}
using TaskFn=void(*)(void*);
struct Job {TaskFn entry;void* arg;uintptr_t task;std::string name;};std::vector<Job> jobs;bool fail_create=false;uintptr_t next_task=1000;
BaseType_t xTaskCreatePinnedToCore(TaskFn fn,const char* name,uint32_t,void* arg,UBaseType_t,TaskHandle_t* handle,BaseType_t){if(fail_create)return pdFAIL;auto id=next_task++;if(handle)*handle=reinterpret_cast<void*>(id);jobs.push_back({fn,arg,id,name});return pdPASS;}
BaseType_t xTaskCreate(TaskFn fn,const char* name,uint32_t stack,void* arg,UBaseType_t priority,TaskHandle_t* handle){return xTaskCreatePinnedToCore(fn,name,stack,arg,priority,handle,tskNO_AFFINITY);}
#include <cuckoo_action_gate.h>
constexpr int LED_A_GPIO=101,LED_B_GPIO=102,SERVO_CENTER_ANGLE=90;
constexpr int MAIN_DOOR_OPEN_SPEED=50,MAIN_DOOR_CLOSE_SPEED=50,MAIN_DOOR_TIME_MS=1400;
constexpr int DOG_SPEED_PERCENT=30,DOG_WALK_TIME_MS=1000,DANCE_SPEED_PERCENT=80;
constexpr int VIOLIN_WARMUP_PERCENT=60,VIOLIN_SPEED_PERCENT=100,VIOLIN_BALANCE_TIME_MS=1200,WATER_WHEEL_SPEED=100;
constexpr int LINDA_SONG_BASE=3001,LINDA_SONG_COUNT=1,BIRD_DOOR_OPEN_SPEED=50,BIRD_DOOR_CLOSE_SPEED=50,BIRD_DOOR_TIME_MS=500,MOTOR_WATER_BIRD_IN2=901;
constexpr int CUCKOO_WAKE_SOUND_SAMPLE_RATE=16000,CUCKOO_WAKE_SOUND_NUM_SAMPLES=2,CUCKOO_BELL_SOUND_SAMPLE_RATE=16000,CUCKOO_BELL_SOUND_NUM_SAMPLES=2; const int16_t cuckoo_wake_sound[2]={0,0},cuckoo_bell_sound[2]={0,0};
constexpr int kDeviceStateIdle=0,kDeviceStateSpeaking=1,kDeviceStateListening=2,kAbortReasonNone=0;
struct Event {long long ms;uintptr_t task;std::string kind;int channel,value;};
std::vector<Event> events;std::map<int,int> levels;long long clock_ms=0;bool stopped=false,bell_seen=false;std::function<void(int)> on_delay;std::function<void()> on_ticks,on_decode;int tick_reads=0,bird_calls=0,new_workers=0;std::set<void*> pcm_allocations;
void record(std::string kind,int channel,int value){levels[channel]=value;if(stopped)events.push_back({clock_ms,task_id,kind,channel,value});}
void gpio_set_level(int pin,int value){record("gpio",pin,value);}
void ledc_set_duty(int,int channel,uint32_t duty){record("pwm",channel,(int)duty);}
void ledc_update_duty(int,int){}
unsigned long xTaskGetTickCount(){++tick_reads;if(on_ticks)on_ticks();return (unsigned long)clock_ms;}
int64_t esp_timer_get_time(){return clock_ms*1000;}
unsigned esp_random(){return 0;}
void vTaskDelay(TickType_t ms){clock_ms+=ms;if(on_delay)on_delay((int)ms);}
void heap_caps_free(void* p){if(p){pcm_allocations.erase(p);std::free(p);}}
struct AudioService {bool bg=false,voice=true,wake=true;int resets=0,clears=0,pushed=0,mixes=0,outputs=0;size_t fill=0;bool IsBgAudioActive(){return bg;}void EnableVoiceProcessing(bool b){voice=b;}void EnableWakeWordDetection(bool b){wake=b;}void RefreshOutputTimestamp(){}void RefreshInputTimestamp(){}void SetWakeWordThreshold(float){}void EnableBgAudioDrain(bool){}void SetOutputMuted(bool){}void ResetDecoder(){++resets;}void ClearBackgroundAudio(){bg=false;++clears;}void SetBackgroundAudioGain(float){bg=true;}size_t GetBgAudioFillLevel(){return fill;}void MixIntoBackgroundAudio(const int16_t*,size_t,float){++mixes;}void OutputRawPcm(const int16_t*,size_t,int){bell_seen=true;++outputs;}void PushBackgroundAudio(const int16_t*,size_t,int){++pushed;} };
struct Application {AudioService audio;int state=kDeviceStateIdle;static Application& GetInstance(){static Application a;return a;}AudioService& GetAudioService(){return audio;}int GetDeviceState(){return state;}void SetDeviceState(int s){state=s;}void Schedule(std::function<void()> f){f();}void AbortSpeaking(int){state=kDeviceStateIdle;}};
void ConvertToPcmUrl(char*,size_t){}
struct Mp3Player {int kind=0;bool HasBackgroundPlayback(){return kind==2&&playing;}bool HasChimePlayback(){return kind==1&&playing;}bool PlayBackground(int,std::function<bool()>,std::function<void()>,std::function<bool()>){kind=2;playing=!fail_create;return playing;}std::string last_url;int starts=0;int PlayOpus(const char* url){last_url=url;++starts;playing=true;return 0;}std::atomic<bool> stop_requested_{false};void PlayAlarmRing(float=1.0f);bool playing=false,requested=false;int stops=0;bool IsPlaying(){return playing;}void Stop(){kind=0;playing=false;requested=true;++stops;}void RequestStop(){kind=0;requested=true;stop_requested_=true;}int DecodeStreaming(int,std::function<void(const int16_t*,size_t,int,void*)> callback,void* ctx,std::function<bool()> keep={}){if(on_decode){auto hook=on_decode;on_decode={};hook();}int16_t pcm[2]={0,0};if(!keep||keep())callback(pcm,2,32000,ctx);return 0;}void SetDisableDucking(bool){}void PlayBgMusic(int){playing=true;}void PlayIndex(int){playing=true;kind=1;}int DecodeToBuffer(int,int16_t** p,size_t* samples,int* sr){*p=(int16_t*)std::malloc(16*sizeof(int16_t));pcm_allocations.insert(*p);*samples=16;*sr=32000;return 0;}};
struct BellSoundPlayer {void PlayCuckooSoundSync();void PlayBellSoundSync();void PlayCuckooSoundAsync();};
class Motor {public:int id=0,in1_pin_=0,in2_pin_=1;bool use_pwm_=false;uint32_t max_duty_=255;int speed_mode_=0,ledc_channel_=0,ledc_channel2_=1,ledc_timer_=0;Motor(int i):id(i),in1_pin_(10+i*2),in2_pin_(11+i*2){}void SetSpeed(int);void Stop();void Forward(int speed=100);void Reverse(int speed=100);};
class Servo {public:int pin_=0,angle_=90,ledc_channel_=0,speed_mode_=0;Servo(int c):ledc_channel_(c){}void SetAngle(int);void Sweep(int,int,int);void Release();};
class CuckooStateMachine {public:
enum PerformanceType{kPerformanceNone,kPerformanceManual,kPerformanceHour,kPerformanceHalf};enum Phase{kPhaseIdle};
Motor* m1_=nullptr;Motor* m2_=nullptr;Motor* m3_=nullptr;Motor* m4_=nullptr;Motor* violin_motor_=nullptr;Motor* water_bird_=nullptr;Servo* violin_servo_=nullptr;Servo* dog_servo_=nullptr;Mp3Player* mp3_=nullptr;BellSoundPlayer* bell_player_=nullptr;
int motor_power_pin_=999;cuckoo_stop::Value<PerformanceType> current_performance_{kPerformanceNone};cuckoo_stop::Value<Phase> current_phase_{kPhaseIdle};
cuckoo_stop::Value<bool> is_running_{false},dog_outro_running_{false},dog_intro_running_{false},dog_intro_done_{false},kids_active_{true},kids_appreciating_{true},door_open_{false},dog_out_{false},kids_dance_{false},flash_leds_active_{false},alarm_stopped_{false},alarm_ringing_{false};
cuckoo_stop::Value<int> music_dance_phase_{0},music_dance_enabled_{-1},m1_music_fwd_count_{0},m1_music_rev_count_{0};cuckoo_stop::Value<bool> dog_outro_done_{false};
cuckoo_stop::Value<unsigned long> m1_fwd_time_{0},m1_rev_time_{0},m1_stage_start_{0};
struct ViolinLoopState {cuckoo_stop::Value<int> stage{0},loop_count{0},timer{0},angle{90},fwd_count{0},rev_count{0};void Reset(){stage=0;loop_count=0;timer=0;angle=90;fwd_count=0;rev_count=0;}}violin_state_;
struct DogTailState {cuckoo_stop::Value<int> angle{15},dir{1},target{65},pause{0};void Reset(){angle=15;dir=1;target=65;pause=0;}}dog_state_;
cuckoo_stop::Value<int> thanks_counter_{0},linda_song_counter_{0},show_music_index_{0},call_count_{0},total_calls_{0};std::atomic<int> current_hour_{12},current_min_{0};std::atomic<bool> hourly_perf_{false};
// REAL_HELPERS
bool IsRunning(){return is_running_.load();}void SaveKidsActive(){}void PlayWavAsset(const char*){}
#if FIXED
bool PlayShowMusicBg(int);
#else
void PlayShowMusicBg(int){}
#endif
void StartShowTask();static void ShowTask(void*){++new_workers;}
std::string music_proxy_host_="fixture.invalid";int music_proxy_port_=80;int PlayOnlineMusic(const char*);int PlayOnlineMusicNow(const char*,uint64_t expected_request=0);void PlayMusic(int);void StopMusic();
void MotorPowerOn();void MotorPowerOff();void ReleaseServos();void StopAll();void Dance();void MotorTest(int);void MusicDanceTick();void MusicDogIntro();void MusicDogOutro();void KidsRest();void CloseDoor();void RunDanceLoop();void RunDanceFinale();void LindaShow();void StartShow();void RunDanceIntro(){}void OpenBirdDoor();void CloseBirdDoor();void BirdJumpPulse();static void PerformanceTask(void*);
};
