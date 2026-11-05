#ifndef MUSIC_H
#define MUSIC_H

#include <string>

class Music {
public:
    virtual ~Music() = default;  // 添加虚析构函数
    
    virtual bool Download(const std::string& song_name, const std::string& artist_name = "") = 0;
    virtual std::string GetDownloadResult() = 0;
    
    // 新增流式播放相关方法
    virtual bool StartStreaming(const std::string& music_url) = 0;
    // take_back=true 表示这是「用户开口要说话」触发的停歌，停完必须把会话接回来
    virtual bool StopStreaming(bool take_back = false) = 0;
    virtual size_t GetBufferSize() const = 0;
    virtual bool IsDownloading() const = 0;
    virtual int16_t* GetAudioData() = 0;
};

#endif // MUSIC_H 
