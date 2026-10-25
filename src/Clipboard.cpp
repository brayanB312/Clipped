#define _CRT_SECURE_NO_WARNINGS
#include "Clipboard.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>

namespace Clipped {

    uint64_t Clipboard::ComputeHash(const void* data, size_t size, bool sampleLargeBuffer) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        uint64_t hash = 14695981039346656037ULL;
        size_t step = (sampleLargeBuffer && size > 4096) ? 64 : 1;
        for (size_t i = 0; i < size; i += step) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static uint64_t HashMemory(const uint8_t* data, size_t size) {
        return Clipboard::ComputeHash(data, size, true);
    }

    // Callback que usa stb_image_write para escribir el PNG directo en nuestro std::vector en RAM
    static void WritePngToMemory(void* context, void* data, int size) {
        auto* buffer = static_cast<std::vector<uint8_t>*>(context);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        buffer->insert(buffer->end(), bytes, bytes + size);
    }

    std::string Clipboard::GetTextUTF8(HWND hwnd) {
        if (!OpenClipboard(hwnd)) return "";

        std::string result = "";
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData != nullptr) {
            const wchar_t* pszText = static_cast<const wchar_t*>(GlobalLock(hData));
            if (pszText != nullptr) {
                int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, pszText, -1, nullptr, 0, nullptr, nullptr);
                if (bytesNeeded > 1) {
                    result.resize(bytesNeeded - 1);
                    WideCharToMultiByte(CP_UTF8, 0, pszText, -1, result.data(), bytesNeeded, nullptr, nullptr);
                }
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
        return result;
    }

    bool Clipboard::SetTextUTF8(HWND hwnd, const std::string& text) {
        if (!OpenClipboard(hwnd)) return false;
        EmptyClipboard();

        // Convertir de UTF-8 (std::string) a UTF-16 (wchar_t) para Windows
        int wideCharsNeeded = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (wideCharsNeeded > 0) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wideCharsNeeded * sizeof(wchar_t));
            if (hMem) {
                wchar_t* pszDest = static_cast<wchar_t*>(GlobalLock(hMem));
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pszDest, wideCharsNeeded);
                GlobalUnlock(hMem);
                if (SetClipboardData(CF_UNICODETEXT, hMem) == nullptr) {
                    GlobalFree(hMem);
                    CloseClipboard();
                    return false;
                }
            }
        }
        CloseClipboard();
        return true;
    }

    bool Clipboard::GetImageAsPNG(HWND hwnd, ImageItem& outImg, uint64_t lastHash) {
        if (!OpenClipboard(hwnd)) return false;

        bool success = false;
        HANDLE hData = GetClipboardData(CF_DIB);
        if (hData != nullptr) {
            const BITMAPINFOHEADER* bih = static_cast<const BITMAPINFOHEADER*>(GlobalLock(hData));
            if (bih != nullptr && (bih->biBitCount == 32 || bih->biBitCount == 24)) {
                int width = bih->biWidth;
                int height = std::abs(bih->biHeight);
                bool isBottomUp = (bih->biHeight > 0); // En Windows >0 significa que viene de cabeza
                int bytesPerPixel = bih->biBitCount / 8;

                size_t rowStride = ((width * bih->biBitCount + 31) / 32) * 4;
                size_t rawSize = rowStride * height;

                // Calcular dónde empiezan los píxeles (saltando máscaras de color si es BI_BITFIELDS)
                size_t headerOffset = bih->biSize;
                if (bih->biCompression == BI_BITFIELDS) headerOffset += 12;
                const uint8_t* srcPixels = reinterpret_cast<const uint8_t*>(bih) + headerOffset;

                uint64_t currentHash = HashMemory(srcPixels, rawSize);
                outImg.hash = currentHash;
                outImg.width = width;
                outImg.height = height;
                outImg.rawSizeBytes = rawSize;

                // Solo procesamos y comprimimos si es una imagen nueva
                if (currentHash != lastHash) {
                    std::vector<uint8_t> rgbaPixels(width * height * 4);

                    for (int y = 0; y < height; ++y) {
                        // Si viene Bottom-Up, leemos desde la última fila hacia arriba para enderezarla
                        int srcY = isBottomUp ? (height - 1 - y) : y;
                        const uint8_t* srcRow = srcPixels + (srcY * rowStride);
                        uint8_t* dstRow = rgbaPixels.data() + (y * width * 4);

                        for (int x = 0; x < width; ++x) {
                            // Convertir BGRA (Windows) -> RGBA (Estándar PNG / DirectX)
                            dstRow[x * 4 + 0] = srcRow[x * bytesPerPixel + 2]; // R
                            dstRow[x * 4 + 1] = srcRow[x * bytesPerPixel + 1]; // G
                            dstRow[x * 4 + 2] = srcRow[x * bytesPerPixel + 0]; // B
                            dstRow[x * 4 + 3] = (bytesPerPixel == 4) ? srcRow[x * 4 + 3] : 255; // A
                        }
                    }

                    // Comprimir a PNG en memoria
                    outImg.pngBytes.clear();
                    outImg.pngBytes.reserve(rawSize / 10);
                    stbi_write_png_to_func(WritePngToMemory, &outImg.pngBytes, width, height, 4, rgbaPixels.data(), width * 4);
                }
                success = true;
            }
            GlobalUnlock(hData);
        }
        CloseClipboard();
        return success;
    }

    bool Clipboard::SetImageFromPNG(HWND hwnd, const std::vector<uint8_t>& pngBytes) {
        if (pngBytes.empty()) return false;

        int width = 0;
        int height = 0;
        int channels = 0;
        uint8_t* rgbaPixels = stbi_load_from_memory(
            pngBytes.data(),
            static_cast<int>(pngBytes.size()),
            &width, &height, &channels, 4
        );
        if (!rgbaPixels || width <= 0 || height <= 0) {
            if (rgbaPixels) stbi_image_free(rgbaPixels);
            return false;
        }

        const size_t rowStride = static_cast<size_t>(width) * 4;
        const size_t pixelDataSize = rowStride * static_cast<size_t>(height);
        const size_t totalSize = sizeof(BITMAPINFOHEADER) + pixelDataSize;

        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, totalSize);
        if (!hMem) {
            stbi_image_free(rgbaPixels);
            return false;
        }

        void* lockedMem = GlobalLock(hMem);
        if (!lockedMem) {
            GlobalFree(hMem);
            stbi_image_free(rgbaPixels);
            return false;
        }

        auto* bih = static_cast<BITMAPINFOHEADER*>(lockedMem);
        ZeroMemory(bih, sizeof(BITMAPINFOHEADER));
        bih->biSize = sizeof(BITMAPINFOHEADER);
        bih->biWidth = width;
        bih->biHeight = height; // Positivo = Bottom-Up (estándar CF_DIB en Windows)
        bih->biPlanes = 1;
        bih->biBitCount = 32;
        bih->biCompression = BI_RGB;
        bih->biSizeImage = static_cast<DWORD>(pixelDataSize);

        uint8_t* dstPixels = reinterpret_cast<uint8_t*>(bih) + sizeof(BITMAPINFOHEADER);

        for (int y = 0; y < height; ++y) {
            // Invertir filas: Top-Down (stb_image) -> Bottom-Up (CF_DIB)
            const uint8_t* srcRow = rgbaPixels + (static_cast<size_t>(y) * rowStride);
            uint8_t* dstRow = dstPixels + (static_cast<size_t>(height - 1 - y) * rowStride);

            for (int x = 0; x < width; ++x) {
                // Convertir RGBA -> BGRA
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R
                dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
            }
        }

        GlobalUnlock(hMem);
        stbi_image_free(rgbaPixels);

        if (!OpenClipboard(hwnd)) {
            GlobalFree(hMem);
            return false;
        }

        EmptyClipboard(); // Asigna a 'hwnd' como propietario actual (GetClipboardOwner() == hwnd)
        if (SetClipboardData(CF_DIB, hMem) == nullptr) {
            GlobalFree(hMem);
            CloseClipboard();
            return false;
        }

        CloseClipboard();
        return true;
    }

}