#pragma once
namespace harness {
inline const char*& PreviewConnectionLabel(){static const char* label="接收（未连接）";return label;}
inline const char* ConnectionStatusLabel(){return PreviewConnectionLabel();}
}
