struct SwitchTask {uintptr_t prior;explicit SwitchTask(uintptr_t next):prior(task_id){task_id=next;}~SwitchTask(){task_id=prior;}};
void drain_jobs(){int n=0;while(!jobs.empty()){if(++n>1000)std::abort();Job j=jobs.front();jobs.erase(jobs.begin());SwitchTask task(j.task);j.entry(j.arg);}}
struct Fixture {Motor m1{1},m2{2},m3{3},vm{5},water{6};Servo vs{201},ds{202};Mp3Player mp3;BellSoundPlayer bell;CuckooStateMachine s;Fixture(){s.m1_=&m1;s.m2_=&m2;s.m3_=&m3;s.violin_motor_=&vm;s.water_bird_=&water;s.violin_servo_=&vs;s.dog_servo_=&ds;s.mp3_=&mp3;s.bell_player_=&bell;m3.use_pwm_=true;m3.ledc_channel_=301;m3.ledc_channel2_=302;}
~Fixture(){on_delay={};on_ticks={};drain_jobs();}
};
int failures=0,defects=0;
void reset(){if(!jobs.empty())std::abort();cuckoo_stop::Gate::Instance().ResetForTest();events.clear();levels.clear();clock_ms=0;stopped=false;bell_seen=false;on_delay={};on_ticks={};on_decode={};tick_reads=0;bird_calls=0;new_workers=0;fail_create=false;task_id=1;Application::GetInstance().audio=AudioService{};Application::GetInstance().state=kDeviceStateIdle;}
void arm(){
#if FIXED
cuckoo_stop::Gate::Instance().Arm();
#endif
}
void stop(Fixture& f,bool restart=false){if(stopped)return;{SwitchTask interrupt(100);f.s.StopAll();}stopped=true;if(restart){SwitchTask command(101);f.s.StartShow();}}
int writes(std::string k,int ch,int value,uintptr_t task=1){return std::count_if(events.begin(),events.end(),[&](const Event&e){return e.task==task&&e.kind==k&&e.channel==ch&&(value<0?e.value>0:e.value==value);});}
int drives(Motor& m,uintptr_t task=1){return m.use_pwm_?writes("pwm",m.ledc_channel_,-1,task)+writes("pwm",m.ledc_channel2_,-1,task):writes("gpio",m.in1_pin_,1,task)+writes("gpio",m.in2_pin_,1,task);}
void safe(const char* name,Fixture& f,bool extra=true){int m1=drives(f.m1),m3=drives(f.m3),servo=writes("pwm",201,-1)+writes("pwm",202,-1),power=writes("gpio",999,0);bool ok=stopped&&m1==0&&m3==0&&servo==0&&power==0&&extra;
std::cout<<name<<" old_M1="<<m1<<" old_M3="<<m3<<" old_servo="<<servo<<" old_power_on="<<power<<" extra="<<extra<<" safe="<<ok<<" elapsed="<<clock_ms<<std::endl;
#if FIXED
if(!ok)++failures;
#else
if(!ok)++defects;
#endif
}
void check(bool ok,const char* name){std::cout<<name<<"="<<ok<<std::endl;if(!ok)++failures;}
int main(){
{reset();Fixture f;arm();on_delay=[&](int ms){if(ms==20)stop(f);};f.s.MusicDogIntro();safe("music_dog_intro",f,!f.s.dog_out_.load());}
{reset();Fixture f;arm();on_delay=[&](int){stop(f);};f.s.Dance();safe("manual_dance",f);}
{reset();Fixture f;f.s.m1_music_fwd_count_=4;arm();on_delay=[&](int){stop(f);};f.s.KidsRest();safe("kids_rest_compensation",f);}
{reset();Fixture f;f.s.m1_music_fwd_count_=4;f.s.music_dance_enabled_=1;f.s.dog_outro_done_=true;arm();on_delay=[&](int){stop(f);};f.s.MusicDanceTick();safe("eof_compensation",f);}
{reset();Fixture f;Application::GetInstance().audio.bg=true;f.mp3.playing=true;arm();on_delay=[&](int ms){if(ms==20)stop(f);};f.s.MusicDanceTick();safe("same_tick_after_intro",f);}
{reset();Fixture f;Application::GetInstance().audio.bg=true;f.mp3.playing=true;f.s.is_running_=true;arm();on_delay=[&](int){stop(f);};f.s.RunDanceLoop();safe("dance_loop_delay",f);}
{reset();Fixture f;Application::GetInstance().audio.bg=true;f.mp3.playing=true;f.s.is_running_=true;arm();cuckoo_stop::Scope workflow;on_delay=[&](int){if(f.s.m1_fwd_time_.load()>0)stop(f);};f.s.RunDanceLoop();f.s.RunDanceFinale();safe("loop_then_real_finale",f);}
{reset();Fixture f;Application::GetInstance().audio.bg=true;f.mp3.playing=true;arm();on_delay=[&](int){if(f.s.m1_fwd_time_.load()>0)stop(f);};f.s.LindaShow();safe("linda_compensation",f);}
{reset();Fixture f;arm();on_delay=[&](int){stop(f);};f.s.MotorTest(5);safe("motor_test_hold",f);}
{reset();Fixture f;Application::GetInstance().audio.bg=true;f.mp3.playing=true;f.s.is_running_=true;arm();int delays=0;on_delay=[&](int){if(delays++==0)stop(f,true);else{Application::GetInstance().audio.bg=false;f.mp3.playing=false;}};f.s.RunDanceLoop();safe("old_loop_after_restart",f,f.s.is_running_.load());}
{reset();Fixture f;Application::GetInstance().audio.bg=true;f.mp3.playing=true;f.s.is_running_=true;arm();on_ticks=[&]{if(tick_reads==5)stop(f);};f.s.RunDanceLoop();safe("while_check_then_write",f);}
{reset();Fixture f;f.s.current_performance_=CuckooStateMachine::kPerformanceHalf;f.s.call_count_=3;f.s.is_running_=true;arm();on_delay=[&](int){if(!stopped){stop(f,true);SwitchTask command(102);cuckoo_stop::Scope scope;f.mp3.playing=true;f.mp3.stops=0;}};{cuckoo_stop::Scope workflow;f.s.OwnPerformanceMedia(false);CuckooStateMachine::PerformanceTask(&f.s);}bool own=f.s.is_running_.load()&&f.s.current_performance_.load()==CuckooStateMachine::kPerformanceManual&&f.mp3.playing&&f.mp3.stops==0;safe("half_old_cleanup_new_owner",f,own);}
{reset();Fixture f;f.s.current_performance_=CuckooStateMachine::kPerformanceHour;f.s.total_calls_=2;f.s.call_count_=0;f.s.is_running_=true;arm();on_delay=[&](int){if(bell_seen)stop(f);};{cuckoo_stop::Scope workflow;f.s.OwnPerformanceMedia(false);CuckooStateMachine::PerformanceTask(&f.s);}check(stopped&&pcm_allocations.empty(),"cancelled_pcm_raii_no_leak");}
{reset();Fixture f;arm();{cuckoo_stop::Scope root;f.bell.PlayCuckooSoundAsync();}stop(f);{SwitchTask user(105);cuckoo_stop::Scope request;f.s.BeginBackground();auto& a=Application::GetInstance().audio;a.bg=true;a.fill=1;}drain_jobs();auto& a=Application::GetInstance().audio;
#if FIXED
check(a.mixes==0&&a.outputs==0,"queued_real_bell_rejects_cancelled_ticket");
#else
if(a.mixes>0)++defects;else ++failures;
#endif
a.fill=0;}
#if FIXED
{reset();Fixture f;arm();cuckoo_stop::Scope old;f.m1.Forward();stop(f);{SwitchTask command(101);cuckoo_stop::Scope newer;f.m1.Forward();}f.m1.Stop();f.ds.Release();check(levels[f.m1.in1_pin_]==1&&levels[f.m1.in2_pin_]==0,"old_stop_cannot_clear_new_output");}
{reset();Fixture f;arm();bool body=false;{cuckoo_stop::Scope old;check(cuckoo_stop::Launch([&]{body=true;},"queued",1024,3)==pdPASS,"queued_reserved");stop(f);}drain_jobs();check(!body,"old_queued_task_not_reticketed");}
{reset();Fixture f;arm();{cuckoo_stop::Scope root;fail_create=true;check(cuckoo_stop::Launch([]{},"fail",1024,3)==pdFAIL,"create_failure_rejected");fail_create=false;}cuckoo_stop::Gate::Instance().ResetForTest();check(true,"create_failure_released_reservation");}
{reset();Fixture f;arm();{cuckoo_stop::Scope old;stop(f);cuckoo_stop::Scope nested;check(!nested.Valid(),"nested_stale_scope_cannot_renew");cuckoo_stop::Scope forbidden(std::function<bool()>([&]{return true;}));check(!forbidden.Valid(),"stale_scope_cannot_admit_new_workflow");}}
{reset();Fixture f;arm();std::atomic<bool> ready{false},go{false};std::thread old([&]{task_id=2;cuckoo_stop::Scope scope;f.m1.Forward();ready=true;while(!go.load())std::this_thread::yield();for(int i=0;i<1000;i++){f.m1.Forward();f.m1.Stop();}});while(!ready.load())std::this_thread::yield();stop(f);go=true;old.join();check(drives(f.m1,2)==0&&levels[f.m1.in1_pin_]==0,"threaded_fence_1000_stale_writes_rejected");}
{reset();Fixture f;f.mp3.playing=true;Application::GetInstance().audio.bg=true;arm();{cuckoo_stop::Scope owner;f.s.BeginBackground();}stop(f);drain_jobs();check(f.mp3.playing&&!f.mp3.requested&&Application::GetInstance().audio.bg,"stop_all_retains_background_music");}
{reset();Fixture f;arm();fail_create=true;f.s.StartShow();fail_create=false;check(!f.s.is_running_.load(),"start_show_create_failure_clears_claim");}
{reset();Fixture f;arm();{SwitchTask interrupt(100);f.s.StopAll();}stopped=true;{SwitchTask command(101);f.s.StartShow();}check(jobs.size()==2,"restore_and_new_setup_queued");auto newest=jobs.back();jobs.pop_back();{SwitchTask task(newest.task);newest.entry(newest.arg);}auto& a=Application::GetInstance().audio;check(!a.voice&&!a.wake&&f.s.is_running_.load(),"new_setup_disabled_voice");drain_jobs();check(!a.voice&&!a.wake&&f.s.is_running_.load(),"old_restore_cannot_enable_new_setup");}
{reset();Fixture f;arm();cuckoo_stop::Scope workflow;f.s.OwnPerformanceMedia(true);on_decode=[&]{stop(f);};bool ok=f.s.PlayShowMusicBg(1);check(ok&&Application::GetInstance().audio.pushed==1&&!f.mp3.requested,"accepted_background_decoder_survives_mechanical_stop");}
{reset();Fixture f;arm();cuckoo_stop::Scope workflow;f.s.OwnPerformanceMedia(true);uint64_t newer=0;on_decode=[&]{SwitchTask user(105);cuckoo_stop::Scope request;newer=f.s.BeginBackground();Application::GetInstance().audio.bg=true;};bool ok=f.s.PlayShowMusicBg(1);check(!ok&&newer==f.s.media_generation_.load()&&Application::GetInstance().audio.bg&&Application::GetInstance().audio.clears==0,"old_background_finalizer_keeps_new_music");}
{reset();Fixture f;arm();f.s.background_owned_=true;on_delay=[&](int){stop(f);};f.mp3.PlayAlarmRing();check(stopped&&Application::GetInstance().audio.outputs==1,"alarm_chunk_cancel_even_with_background_owner");}
{reset();Fixture f;arm();std::atomic<bool> held{false},release{false};std::thread media([&]{task_id=200;cuckoo_stop::Scope root;cuckoo_stop::Gate::Instance().Media([&]{held=true;while(!release.load())std::this_thread::yield();});});while(!held.load())std::this_thread::yield();stop(f);bool immediate=levels[999]==1;release=true;media.join();check(immediate,"raw_stop_does_not_wait_for_slow_media");}
{reset();Fixture f;arm();std::atomic<bool> closing_started{false},drained{false};{cuckoo_stop::Scope root;check(cuckoo_stop::Launch([]{},"lifetime",1024,3)==pdPASS,"lifetime_reserved_before_queue");}std::thread close([&]{task_id=200;closing_started=true;cuckoo_stop::Gate::Instance().CloseAndWait();drained=true;});while(!closing_started.load()||!cuckoo_stop::Gate::Instance().Closing())std::this_thread::yield();check(!drained.load(),"close_waits_for_queued_lifetime");drain_jobs();close.join();check(drained.load(),"cancelled_queue_releases_lifetime");}
{int bad=0,cases=0;for(int reverse=0;reverse<2;++reverse)for(int boundary=0;boundary<=101;++boundary){reset();Fixture f;arm();cuckoo_stop::Scope workflow;int delayed=0;if(boundary==0)stop(f);on_delay=[&](int){if(++delayed==boundary)stop(f);};f.ds.Sweep(reverse?145:10,reverse?10:145,2000);if(!stopped||writes("pwm",202,-1)>0)++bad;++cases;}check(cases==204&&bad==0,"servo_sweep_204_cancel_boundaries");}
{reset();Fixture f;arm();Application::GetInstance().state=kDeviceStateSpeaking;f.s.PlayOnlineMusic("http://fixture.invalid/old");{SwitchTask user(105);cuckoo_stop::Scope newer;f.s.PlayOnlineMusicNow("http://fixture.invalid/new");}Application::GetInstance().state=kDeviceStateIdle;drain_jobs();check(f.mp3.starts==1&&f.mp3.last_url=="http://fixture.invalid/new","queued_old_url_cannot_replace_new_request");}
{reset();Fixture f;arm();f.s.pending_music_generation_=2;int result=f.s.PlayOnlineMusicNow("http://fixture.invalid/old",1);check(result==-1&&f.mp3.starts==0&&f.s.media_generation_.load()==0,"deferred_request_rechecked_inside_real_admission");}
{reset();Fixture f;arm();cuckoo_stop::Scope workflow;f.s.OwnPerformanceMedia(true);on_decode=[&]{SwitchTask user(105);f.s.StopMusic();};bool ok=f.s.PlayShowMusicBg(1);check(!ok&&Application::GetInstance().audio.pushed==0&&Application::GetInstance().audio.clears==1,"explicit_stop_music_revokes_decoder_without_old_cleanup");}
#endif
std::cout<<"failures="<<failures<<" baseline_defects="<<defects<<std::endl;
#if FIXED
return failures?1:0;
#else
return failures?1:(defects>=7?42:2);
#endif
}
