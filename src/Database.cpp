#include "Database.h"
#include "sqlite3.h"
#include <chrono>

namespace Clipped {

    Database::~Database() {
        Close();
    }

    int64_t Database::CurrentTimestampMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    bool Database::Init(const std::string& dbPath, int maxUnpinnedItems) {
        Close();
        m_maxUnpinnedItems = (maxUnpinnedItems > 0) ? maxUnpinnedItems : 300;

        if (sqlite3_open(dbPath.c_str(), &m_db) != SQLITE_OK) {
            Close();
            return false;
        }

        // Configuración de bajo consumo de RAM (< 1 MB para la caché de páginas de SQLite) y velocidad WAL
        const char* initSql = R"SQL(
            PRAGMA journal_mode = WAL;
            PRAGMA synchronous = NORMAL;
            PRAGMA temp_store = MEMORY;
            PRAGMA cache_size = -512;

            CREATE TABLE IF NOT EXISTS clips (
                id           INTEGER PRIMARY KEY AUTOINCREMENT,
                type         INTEGER NOT NULL,
                text_content TEXT,
                image_blob   BLOB,
                width        INTEGER DEFAULT 0,
                height       INTEGER DEFAULT 0,
                hash         INTEGER NOT NULL UNIQUE,
                created_at   INTEGER NOT NULL,
                is_pinned    INTEGER NOT NULL DEFAULT 0
            );

            CREATE INDEX IF NOT EXISTS idx_clips_order ON clips(is_pinned DESC, created_at DESC);
        )SQL";

        char* errMsg = nullptr;
        if (sqlite3_exec(m_db, initSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            if (errMsg) sqlite3_free(errMsg);
            Close();
            return false;
        }

        return true;
    }

    void Database::Close() {
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
    }

    bool Database::SaveText(const std::string& text) {
        if (!m_db || text.empty()) return false;

        const uint64_t hash = Clipboard::ComputeHash(text.data(), text.size(), false);
        const int64_t nowMs = CurrentTimestampMs();

        const char* sql = R"SQL(
            INSERT INTO clips (type, text_content, image_blob, width, height, hash, created_at, is_pinned)
            VALUES (?1, ?2, NULL, 0, 0, ?3, ?4, 0)
            ON CONFLICT(hash) DO UPDATE SET
                text_content = excluded.text_content,
                created_at = excluded.created_at;
        )SQL";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int(stmt, 1, static_cast<int>(ClipType::Text));
        sqlite3_bind_text(stmt, 2, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(hash));
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(nowMs));

        const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);

        if (ok) {
            PruneExcessClips();
        }
        return ok;
    }

    bool Database::SaveImage(const ImageItem& img) {
        if (!m_db || img.hash == 0) return false;

        const int64_t nowMs = CurrentTimestampMs();

        // Si pngBytes viene vacío (mismo hash que lastHash en GetImageAsPNG), solo actualizamos created_at
        if (img.pngBytes.empty()) {
            const char* bumpSql = "UPDATE clips SET created_at = ?1 WHERE hash = ?2;";
            sqlite3_stmt* bumpStmt = nullptr;
            if (sqlite3_prepare_v2(m_db, bumpSql, -1, &bumpStmt, nullptr) != SQLITE_OK) {
                return false;
            }
            sqlite3_bind_int64(bumpStmt, 1, static_cast<sqlite3_int64>(nowMs));
            sqlite3_bind_int64(bumpStmt, 2, static_cast<sqlite3_int64>(img.hash));
            const bool ok = (sqlite3_step(bumpStmt) == SQLITE_DONE);
            sqlite3_finalize(bumpStmt);
            return ok;
        }

        const char* sql = R"SQL(
            INSERT INTO clips (type, text_content, image_blob, width, height, hash, created_at, is_pinned)
            VALUES (?1, NULL, ?2, ?3, ?4, ?5, ?6, 0)
            ON CONFLICT(hash) DO UPDATE SET
                created_at = excluded.created_at;
        )SQL";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int(stmt, 1, static_cast<int>(ClipType::Image));
        sqlite3_bind_blob(stmt, 2, img.pngBytes.data(), static_cast<int>(img.pngBytes.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, img.width);
        sqlite3_bind_int(stmt, 4, img.height);
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(img.hash));
        sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(nowMs));

        const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);

        if (ok) {
            PruneExcessClips();
        }
        return ok;
    }

    bool Database::TouchClip(int64_t id) {
        if (!m_db) return false;

        const char* sql = "UPDATE clips SET created_at = ?1 WHERE id = ?2;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(CurrentTimestampMs()));
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(id));

        const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }

    bool Database::SetPinned(int64_t id, bool pinned) {
        if (!m_db) return false;

        const char* sql = "UPDATE clips SET is_pinned = ?1 WHERE id = ?2;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int(stmt, 1, pinned ? 1 : 0);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(id));

        const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }

    bool Database::DeleteClip(int64_t id) {
        if (!m_db) return false;

        const char* sql = "DELETE FROM clips WHERE id = ?1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(id));

        const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }

    bool Database::ClearUnpinned() {
        if (!m_db) return false;
        char* errMsg = nullptr;
        const bool ok = (sqlite3_exec(m_db, "DELETE FROM clips WHERE is_pinned = 0;", nullptr, nullptr, &errMsg) == SQLITE_OK);
        if (errMsg) sqlite3_free(errMsg);
        return ok;
    }

    std::vector<ClipRecord> Database::GetClips(const std::string& searchQuery, int limit) {
        std::vector<ClipRecord> results;
        if (!m_db || limit <= 0) return results;

        const bool hasFilter = !searchQuery.empty();
        const char* sqlAll = R"SQL(
            SELECT id, type, text_content, image_blob, width, height, hash, created_at, is_pinned
            FROM clips
            ORDER BY is_pinned DESC, created_at DESC
            LIMIT ?1;
        )SQL";

        const char* sqlFiltered = R"SQL(
            SELECT id, type, text_content, image_blob, width, height, hash, created_at, is_pinned
            FROM clips
            WHERE type = 0 AND text_content LIKE ?1
            ORDER BY is_pinned DESC, created_at DESC
            LIMIT ?2;
        )SQL";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, hasFilter ? sqlFiltered : sqlAll, -1, &stmt, nullptr) != SQLITE_OK) {
            return results;
        }

        if (hasFilter) {
            std::string pattern = "%" + searchQuery + "%";
            sqlite3_bind_text(stmt, 1, pattern.c_str(), static_cast<int>(pattern.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, limit);
        } else {
            sqlite3_bind_int(stmt, 1, limit);
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ClipRecord rec;
            rec.id = sqlite3_column_int64(stmt, 0);
            rec.type = static_cast<ClipType>(sqlite3_column_int(stmt, 1));

            if (rec.type == ClipType::Text) {
                const unsigned char* textPtr = sqlite3_column_text(stmt, 2);
                const int textBytes = sqlite3_column_bytes(stmt, 2);
                if (textPtr && textBytes > 0) {
                    rec.textContent.assign(reinterpret_cast<const char*>(textPtr), static_cast<size_t>(textBytes));
                }
            } else if (rec.type == ClipType::Image) {
                const void* blobPtr = sqlite3_column_blob(stmt, 3);
                const int blobBytes = sqlite3_column_bytes(stmt, 3);
                if (blobPtr && blobBytes > 0) {
                    const auto* bytes = static_cast<const uint8_t*>(blobPtr);
                    rec.imageBlob.assign(bytes, bytes + blobBytes);
                }
            }

            rec.width = sqlite3_column_int(stmt, 4);
            rec.height = sqlite3_column_int(stmt, 5);
            rec.hash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
            rec.createdAt = sqlite3_column_int64(stmt, 7);
            rec.isPinned = (sqlite3_column_int(stmt, 8) != 0);

            results.push_back(std::move(rec));
        }

        sqlite3_finalize(stmt);
        return results;
    }

    int Database::GetTotalCount() {
        if (!m_db) return 0;

        const char* sql = "SELECT COUNT(*) FROM clips;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return 0;
        }

        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return count;
    }

    void Database::ReleaseMemory() {
        if (m_db) {
            sqlite3_db_release_memory(m_db);
        }
    }

    void Database::PruneExcessClips() {
        if (!m_db || m_maxUnpinnedItems <= 0) return;

        const char* sql = R"SQL(
            DELETE FROM clips
            WHERE is_pinned = 0
              AND id NOT IN (
                  SELECT id FROM clips
                  WHERE is_pinned = 0
                  ORDER BY created_at DESC
                  LIMIT ?1
              );
        )SQL";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return;
        }

        sqlite3_bind_int(stmt, 1, m_maxUnpinnedItems);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

}
