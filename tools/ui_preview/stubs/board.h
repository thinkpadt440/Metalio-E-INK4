#pragma once
class Board { public: static Board& GetInstance() { static Board b; return b; } bool IsNetworkReady() { return true; } };
