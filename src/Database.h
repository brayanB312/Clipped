#pragma once
#include "Clipboard.h"
#include <string>
#include <vector>
#include <cstdint>

struct sqlite3;

namespace Clipped {

    enum class ClipType : int {
        Text = 0,
        Image = 1
    };

    struct ClipRecord {
        int64_t id = 0;
        ClipType type = ClipType::Text;
        std::string textContent;
        std::vector<uint8_t> imageBlob; // PNG comprimido en bytes si type == ClipType::Image
        int width = 0;
        int height = 0;
        uint64_t hash = 0;
        int64_t createdAt = 0; // Unix epoch en milisegundos
        bool isPinned = false;
    };

    class Database {
    public:
        Database() = default;
        ~Database();

        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;

        // Abre o crea el archivo SQLite y prepara la tabla 'clips' e índices
        bool Init(const std::string& dbPath = "clipped.db", int maxUnpinnedItems = 300);
        void Close();

        // Inserta un texto o actualiza su created_at si su hash ya existe
        bool SaveText(const std::string& text);

        // Inserta una imagen PNG o actualiza su created_at si su hash ya existe
        bool SaveImage(const ImageItem& img);

        // Actualiza created_at al instante actual (al restaurar un ítem desde el historial)
        bool TouchClip(int64_t id);

        // Fija o desfija un ítem para que no sea borrado por el límite automático
        bool SetPinned(int64_t id, bool pinned);

        // Elimina un ítem específico
        bool DeleteClip(int64_t id);

        // Elimina todos los clips que no estén fijados (is_pinned = 0)
        bool ClearUnpinned();

        // Consulta el historial ordenado por (is_pinned DESC, created_at DESC)
        // Si searchQuery no está vacío, filtra por text_content LIKE %searchQuery%
        std::vector<ClipRecord> GetClips(const std::string& searchQuery = "", int limit = 100);

        // Obtiene el número total de registros almacenados
        int GetTotalCount();

        // Libera páginas no esenciales de la caché de SQLite para minimizar RAM en reposo
        void ReleaseMemory();

    private:
        void PruneExcessClips();
        static int64_t CurrentTimestampMs();

        sqlite3* m_db = nullptr;
        int m_maxUnpinnedItems = 300;
    };

}
