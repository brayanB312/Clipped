#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace Clipped {

    struct ImageItem {
        int width = 0;
        int height = 0;
        uint64_t hash = 0;
        size_t rawSizeBytes = 0;
        std::vector<uint8_t> pngBytes; // Imagen ya comprimida en PNG (ocupa 95% menos RAM)
    };

    class Clipboard {
    public:
        // Lee texto UTF-8 del portapapeles
        static std::string GetTextUTF8(HWND hwnd);

        // Escribe texto UTF-8 de vuelta al portapapeles (para cuando transformemos SQL/JSON)
        static bool SetTextUTF8(HWND hwnd, const std::string& text);

        // Lee imagen, endereza las filas (Bottom-Up -> Top-Down), convierte BGRA->RGBA y comprime a PNG
        static bool GetImageAsPNG(HWND hwnd, ImageItem& outImg, uint64_t lastHash);

        // Decodifica un PNG en memoria y lo restaura al portapapeles como CF_DIB (BGRA Bottom-Up)
        static bool SetImageFromPNG(HWND hwnd, const std::vector<uint8_t>& pngBytes);

        // Calcula un hash FNV-1a de 64 bits para deduplicar textos o buffers de imagen
        static uint64_t ComputeHash(const void* data, size_t size, bool sampleLargeBuffer = false);
    };

}