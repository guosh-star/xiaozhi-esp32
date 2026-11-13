struct Assets {
    static Assets& GetInstance() { static Assets assets; return assets; }
    bool GetAssetData(const char* name, void*& data, size_t& bytes);
};
std::vector<std::string> decoded_assets;
bool Assets::GetAssetData(const char* name, void*& data, size_t& bytes) {
    static uint8_t encoded_fixture[32] = {};
    decoded_assets.push_back(name); data = encoded_fixture; bytes = sizeof(encoded_fixture); return true;
}
using esp_audio_err_t = int;
constexpr int ESP_AUDIO_ERR_OK=0, ESP_AUDIO_ERR_BUFF_NOT_ENOUGH=1, ESP_AUDIO_ERR_NOT_SUPPORT=2, ESP_AUDIO_DEC_RECOVERY_NONE=0;
struct esp_audio_dec_info_t {int sample_rate=32000,channel=1,bitrate=32000;};
struct esp_audio_dec_in_raw_t {uint8_t* buffer;size_t len,consumed;int frame_recover;};
struct esp_audio_dec_out_frame_t {uint8_t* buffer;size_t len,needed_size,decoded_size;};
int decoder_handles=0;
int esp_mp3_dec_open(void*,int,void** handle) {*handle=new int(1);++decoder_handles;return 0;}
int esp_mp3_dec_close(void* handle) {delete static_cast<int*>(handle);--decoder_handles;return 0;}
int esp_mp3_dec_reset(void*) {return 0;}
int esp_mp3_dec_decode(void*,esp_audio_dec_in_raw_t* raw,esp_audio_dec_out_frame_t* frame,esp_audio_dec_info_t*) {
    if (on_decode) {auto hook=on_decode;on_decode={};hook();}
    std::fill(frame->buffer,frame->buffer+8,0);raw->consumed=std::min<size_t>(8,raw->len);frame->decoded_size=8;return 0;
}
