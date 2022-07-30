#include "Document.hpp"

#include "Macros.hpp"
#include "Utils.hpp"

#include <date/date.h>
#include <sqlite3.h>
#include <cassert>
#include <sstream>
#include <stdexcept>
#include <string_view>

using namespace std::literals;

// A number for `PRAGMA user_vesrion`, representing the current database version. Increment when the table format changes.
#define CURRENT_DATABASE_VERSION 1
// NOTE: macros for string literal concatenation only
#define ROOT_BULLET_PBID 1
#define ROOT_BULLET_RBID 0
constexpr Ionl::Pbid kRootBulletPbid = ROOT_BULLET_PBID;
constexpr Ionl::Rbid kRootBulletRbid = ROOT_BULLET_RBID;

Ionl::BulletType Ionl::BulletContent::GetType() const {
    return std::visit(
        Overloaded{
            [](const BulletContentTextual&) { return BulletType::Textual; },
            [](const BulletContentMirror&) { return BulletType::Mirror; },
        },
        v);
}

bool Ionl::Bullet::IsRootBullet() const {
    return pbid == kRootBulletPbid;
}

struct SQLiteDatabase {
    sqlite3* database = nullptr;

    ~SQLiteDatabase() {
        // NOTE: calling with NULL is a harmless no-op
        int result = sqlite3_close(database);
        assert(result == SQLITE_OK);
    }

    operator sqlite3*() const { return database; }
    sqlite3** operator&() { return &database; }
};

struct SQLiteStatement {
    sqlite3_stmt* stmt = nullptr;

    SQLiteStatement(const SQLiteStatement&) = delete;
    SQLiteStatement& operator=(const SQLiteStatement&) = delete;

    SQLiteStatement() = default;

    SQLiteStatement(sqlite3* database, std::string_view sql) {
        Initialize(database, sql);
    }

    ~SQLiteStatement() {
        // NOTE: calling with NULL is a harmless no-op
        // NOTE: we don't care about the error code, because they are returned if the statement has errored in the most recent execution
        //       but deleting it will succeeed anyways
        sqlite3_finalize(stmt);
    }

    operator sqlite3_stmt*() const { return stmt; }
    sqlite3_stmt** operator&() { return &stmt; }

    void Initialize(sqlite3* database, std::string_view sql) {
        int result = sqlite3_prepare(database, sql.data(), sql.size(), &stmt, nullptr);
        if (result != SQLITE_OK) {
            std::string msg;
            msg += "Failed to prepare SQLite3 statement, error message:\n";
            msg += sqlite3_errmsg(database);
            throw std::runtime_error(msg);
        }
    }

    bool InitializeLazily(sqlite3* database, std::string_view sql) {
        if (!stmt) {
            Initialize(database, sql);
            return true;
        }
        return false;
    }
};

struct SQLiteRunningStatement {
    sqlite3_stmt* stmt;

    SQLiteRunningStatement(SQLiteStatement& stmt)
        : stmt{ stmt.stmt } {
    }

    ~SQLiteRunningStatement() {
        sqlite3_clear_bindings(stmt);
        sqlite3_reset(stmt);
    }

    void BindArgument(int index, int32_t value) {
        sqlite3_bind_int(stmt, index, (int)value);
    }

    void BindArgument(int index, uint32_t value) {
        sqlite3_bind_int(stmt, index, (int)value);
    }

    void BindArgument(int index, int64_t value) {
        sqlite3_bind_int64(stmt, index, value);
    }

    void BindArgument(int index, uint64_t value) {
        sqlite3_bind_int64(stmt, index, (int64_t)value);
    }

    void BindArgument(int index, const char* value) {
        sqlite3_bind_text(stmt, index, value, -1, nullptr);
    }

    void BindArgument(int index, std::string_view value) {
        sqlite3_bind_text(stmt, index, value.data(), value.size(), nullptr);
    }

    void BindArgument(int index, std::nullptr_t) {
        // Noop
    }

    template <typename... Ts>
    void BindArguments(Ts... args) {
        // NOTE: SQLite3 argument index starts at 1
        size_t idx = 1;
        auto HandleEachArgument = [this, &idx](auto arg) {
            BindArgument(idx, arg);
            ++idx;
        };
        (HandleEachArgument(std::forward<Ts>(args)), ...);
    }

    int Step() {
        return sqlite3_step(stmt);
    }

    void StepAndCheck(int forErrCode) {
        int errCode = sqlite3_step(stmt);
        assert(errCode == forErrCode);
    }

    void StepUntilDoneOrError() {
        while (true) {
            int err = sqlite3_step(stmt);
            // SQLITE_DONE, and all others are error codes
            // SQLITE_OK is never returned for sqlite3_step() //TODO fact check this
            if (err == SQLITE_DONE) {
                break;
            }
            if (err != SQLITE_ROW) {
                std::string msg;
                msg += "Error executing SQLite3 statement, error message:\n";
                msg += sqlite3_errmsg(sqlite3_db_handle(stmt));
                throw std::runtime_error(msg);
                break;
            }
        }
    }

    using TimePoint = std::chrono::time_point<std::chrono::system_clock>;
    using TpFromUnixTimestamp = std::pair<TimePoint, int64_t>;
    using TpFromDateTime = std::pair<TimePoint, const char*>;

    // TODO replace with overloads?
    template <typename T>
    auto ResultColumn(int column) const {
        if constexpr (std::is_enum_v<T>) {
            auto value = sqlite3_column_int64(stmt, column);
            return static_cast<T>(value);
        } else if constexpr (std::is_same_v<T, int>) {
            return sqlite3_column_int(stmt, column);
        } else if constexpr (std::is_same_v<T, int64_t>) {
            return sqlite3_column_int64(stmt, column);
        } else if constexpr (std::is_same_v<T, const char*>) {
            return (const char*)sqlite3_column_text(stmt, column);
        } else if constexpr (std::is_same_v<T, std::string>) {
            auto cstr = (const char*)sqlite3_column_text(stmt, column);
            return std::string(cstr);
        } else if constexpr (std::is_same_v<T, TpFromUnixTimestamp>) {
            auto unixTimestamp = sqlite3_column_int64(stmt, column);
            auto chrono = std::chrono::seconds(unixTimestamp);
            return TimePoint(chrono);
        } else if constexpr (std::is_same_v<T, TpFromDateTime>) {
            auto datetime = (const char*)sqlite3_column_text(stmt, column);
            if (datetime) {
                std::stringstream ss(datetime);
                TimePoint timepoint;
                ss >> date::parse("%F %T", timepoint);
                return timepoint;
            } else {
                return TimePoint();
            }
        } else {
            static_assert(false && sizeof(T), "Unknown type");
        }
    }

    template <typename... Ts>
    auto ResultColumns() {
        // NOTE: SQLite3 column index starts at 0
        // NOTE: ((size_t)-1) + 1 == 0
        size_t idx = -1;
        // NOTE: std::make_tuple() -- variadic template function
        //       std::tuple() -- CTAD constructor
        //       Both of these cause make the comma operator unsequenced, not viable here
        return std::tuple{ (++idx, ResultColumn<Ts>(idx))... };
    }
};

class Ionl::Document::BackingDataStore {
public:
    // NOTE: this must be the first field, because we want it to destruct after all other statement fields
    SQLiteDatabase database;
    SQLiteStatement getBulletContent;
    SQLiteStatement getBulletParent;
    SQLiteStatement getBulletChildren;
    SQLiteStatement insertBullet;
    SQLiteStatement deleteBullet;
    SQLiteStatement setBulletContent;
    SQLiteStatement setBulletPosition;

public:
    BackingDataStore(const char* dbPath) {
        int reuslt = sqlite3_open(dbPath, &database);
        if (reuslt != SQLITE_OK) {
            std::string msg;
            msg += "Failed to open SQLite3 database, error message:\n";
            msg += sqlite3_errmsg(database);
            throw std::runtime_error(msg);
        }

        // NOTE: These pragmas are not persistent, so we need to set them every time
        // As of SQLite3 3.38.5, it defaults to foreign_keys = OFF, so we need this to be on for ON DELETE CASCADE and etc. to work
        sqlite3_exec(database, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);

        {
            SQLiteStatement readVersionStmt;
            readVersionStmt.InitializeLazily(database, "PRAGMA user_version"sv);

            int result = sqlite3_step(readVersionStmt);
            assert(result == SQLITE_ROW);
            int currentDatabaseVersion = sqlite3_column_int(readVersionStmt, 0);

            result = sqlite3_step(readVersionStmt);
            assert(result == SQLITE_DONE);

            if (currentDatabaseVersion == 0) {
                // Newly created database, initialize it
                SetDatabaseUserVersion();
                InitializeTables();
            } else if (currentDatabaseVersion == CURRENT_DATABASE_VERSION) {
                // Same version, no need to do anything
            } else {
                // TODO automatic migration?
                std::string msg;
                msg += "Incompatbile database versions ";
                msg += currentDatabaseVersion;
                msg += " (in file) vs ";
                msg += CURRENT_DATABASE_VERSION;
                msg += " (expected).";
                throw std::runtime_error(msg);
            }
        }

        getBulletContent.Initialize(database, "SELECT CreationTime, ModifyTime, ContentType, ContentValue FROM Bullets WHERE Bullets.Pbid = ?1"sv);
        getBulletParent.Initialize(database, "SELECT ParentPbid FROM Bullets WHERE Bullets.Pbid = ?1"sv);
        getBulletChildren.Initialize(database, R"""(
SELECT Bullets.Pbid FROM Bullets
WHERE Bullets.ParentPbid = ?1
ORDER BY ParentSorting
)"""sv);

        insertBullet.Initialize(database, R"""(
INSERT INTO Bullets(ParentPbid, ParentSorting, CreationTime, ModifyTime)
SELECT ?1, max(ParentSorting) + 1, datetime('now'), datetime('now')
    FROM Bullets
    WHERE ParentPbid = ?1
)"""sv);

        deleteBullet.Initialize(database, R"""(
DELETE FROM Bullets
WHERE Pbid = ?1
)"""sv);

        setBulletContent.Initialize(database, R"""(
UPDATE Bullets
SET ModifyTime = datetime('now'),
    ContentType = ?2,
    ContentValue = ?3
WHERE Pbid = ?1
)"""sv);

        setBulletPosition.Initialize(database, R"""(
UPDATE BULLETS
SET ModifyTime = datetime('now'),
    ParentPbid = ?2,
    ParentSorting = ?3
WHERE Pbid = ?1;

UPDATE BULLETS
SET ParentSorting = ParentSorting + 1
WHERE ParentPbid = ?2
  AND ParentSorting > ?3;
)"""sv);
    }

    Bullet FetchBullet(Pbid pbid) {
        Bullet result;
        result.pbid = pbid;
        result.rbid = (size_t)-1;
        {
            SQLiteRunningStatement rt(getBulletContent);
            rt.BindArguments(pbid);

            rt.StepAndCheck(SQLITE_ROW);

            using Time = SQLiteRunningStatement::TpFromDateTime;
            auto [creationTime, modifyTime, contentType] = rt.ResultColumns<Time, Time, BulletType>();
            result.creationTime = creationTime;
            result.modifyTime = modifyTime;
            switch (contentType) {
                case BulletType::Textual:
                default: {
                    auto content = rt.ResultColumn<const char*>(/*4th*/ 3);
                    result.content.v = BulletContentTextual{
                        .text = content ? content : "",
                    };
                } break;

                case BulletType::Mirror: {
                    auto refereePbid = (Pbid)rt.ResultColumn<int64_t>(/*4th*/ 3);
                    result.content.v = BulletContentMirror{
                        .referee = refereePbid,
                    };
                } break;
            }
        }
        result.parentPbid = FetchParentOfBullet(pbid);
        result.children = FetchChildrenOfBullet(pbid);
        return result;
    }

    Pbid FetchParentOfBullet(Pbid bullet) {
        SQLiteRunningStatement rt(getBulletParent);
        rt.BindArguments(bullet);

        rt.StepAndCheck(SQLITE_ROW);

        auto [parentPbid] = rt.ResultColumns<int64_t>();
        return parentPbid;
    }

    std::vector<Pbid> FetchChildrenOfBullet(Pbid bullet) {
        std::vector<Pbid> result;

        SQLiteRunningStatement rt(getBulletChildren);
        rt.BindArguments(bullet);
        while (true) {
            int err = rt.Step();
            if (err != SQLITE_ROW) {
                break;
            }

            auto [childPbid] = rt.ResultColumns<int64_t>();
            result.push_back(childPbid);
        }

        return result;
    }

    Pbid InsertEmptyBullet() {
        SQLiteRunningStatement rt(insertBullet);
        rt.BindArguments(kRootBulletPbid, nullptr);
        rt.StepUntilDoneOrError();

        return sqlite3_last_insert_rowid(database);
    }

    void DeleteBullet(Pbid bullet) {
        SQLiteRunningStatement rt(deleteBullet);
        rt.BindArguments(bullet);
        rt.StepUntilDoneOrError();
    }

    void SetBulletContent(Pbid bullet, const BulletContent& bulletContent) {
        SQLiteRunningStatement rt(setBulletContent);
        rt.BindArgument(0, bullet);
        std::visit(
            Overloaded{
                [&](const BulletContentTextual& bc) {
                    rt.BindArgument(2, (int)BulletType::Textual);
                    rt.BindArgument(3, bc.text);
                },
                [&](const BulletContentMirror& bc) {
                    rt.BindArgument(2, (int)BulletType::Textual);
                    rt.BindArgument(3, (int64_t)bc.referee);
                },
            },
            bulletContent.v);
        rt.StepUntilDoneOrError();
    }

    void SetBulletPosition(Pbid bullet, Pbid newParent, int newIndex) {
        SQLiteRunningStatement rt(setBulletPosition);
        rt.BindArguments(bullet, newParent, newIndex);
        rt.StepUntilDoneOrError();
    }

private:
    void SetDatabaseUserVersion() {
        char* errMsg = nullptr;
        int result = sqlite3_exec(
            database,
            "PRAGMA user_version = " STRINGIFY(CURRENT_DATABASE_VERSION),
            nullptr,
            nullptr,
            &errMsg);
        assert(result == SQLITE_OK);
    }

    void InitializeTables() {
        char* errMsg = nullptr;
        // clang-format off
        int result = sqlite3_exec(database, R"""(
BEGIN TRANSACTION;
CREATE TABLE Config(
    Key TEXT PRIMARY KEY,
    Value,
    UNIQUE (Key)
);

CREATE TABLE Bullets(
    Pbid INTEGER PRIMARY KEY,
    ParentPbid INTEGER REFERENCES Bullets(Pbid),
    ParentSorting INTEGER,
    CreationTime DATETIME,
    ModifyTime DATETIME,
    -- enum BulletType
    ContentType INTEGER,
    -- If BulletType::Simple, this is TEXT
    -- If BulletType::Reference, this is INTEGER REFERENCES Bullet(Pbid)
    ContentValue
);

CREATE UNIQUE INDEX Idx_Bullets_ParentChild
ON Bullets(ParentPbid, ParentSorting);

CREATE INDEX Idx_Bullets_CreationTime
ON Bullets(CreationTime);

CREATE INDEX Idx_Bullets_ModifyTime
ON Bullets(ModifyTime);
)"""
// Root bullet
// NOTE: all of other fields are left NULL because they are irrelevant
"INSERT INTO BULLETS(Pbid)"
"VALUES (" STRINGIFY(ROOT_BULLET_PBID) ")"
";"
// The default bullet
// TODO maybe we should just let the user create this by pressing enter in the initial empty pane
"INSERT INTO Bullets(ParentPbid, ParentSorting, CreationTime, ModifyTime, ContentType, ContentValue)"
"VALUES (last_insert_rowid(), 0, datetime('now'), datetime('now'), 1, '')"
";"
"COMMIT TRANSACTION;",
            nullptr,
            nullptr,
            &errMsg);
        // clang-format on
        assert(result == SQLITE_OK);
    }
};

Ionl::Document::Document()
    : mStore{ new BackingDataStore("./notebook.sqlite3") } //
{
    // Always load the root bullet
    auto& root = FetchBulletByPbid(kRootBulletPbid);
    assert(root.pbid == kRootBulletPbid);
    assert(root.rbid == kRootBulletRbid);
}

Ionl::Document::~Document() {
    delete mStore;
}

const Ionl::Bullet& Ionl::Document::GetRoot() const {
    return mBullets[0].value();
}

Ionl::Bullet& Ionl::Document::GetRoot() {
    return const_cast<Bullet&>(const_cast<const Document*>(this)->GetRoot());
}

Ionl::Bullet* Ionl::Document::GetBulletByRbid(Rbid rbid) {
    if (rbid >= mBullets.size()) {
        return nullptr;
    }

    auto& ob = mBullets[rbid];
    if (!ob.has_value()) {
        return nullptr;
    }

    return &ob.value();
}

Ionl::Bullet* Ionl::Document::GetBulletByPbid(Pbid pbid) {
    auto iter = mPtoRmap.find(pbid);
    if (iter != mPtoRmap.end()) {
        return GetBulletByRbid(iter->second);
    }

    return nullptr;
}

Ionl::Bullet& Ionl::Document::FetchBulletByPbid(Pbid pbid) {
    auto iter = mPtoRmap.find(pbid);
    if (iter != mPtoRmap.end()) {
        return *GetBulletByRbid(iter->second);
    }

    return *Store(mStore->FetchBullet(pbid));
}

Ionl::Bullet& Ionl::Document::CreateBullet() {
    auto pbid = mStore->InsertEmptyBullet();
    auto& bullet = *Store(mStore->FetchBullet(pbid));
    return bullet;
}

void Ionl::Document::DeleteBullet(Bullet& bullet) {
    mStore->DeleteBullet(bullet.pbid);
    mPtoRmap.erase(bullet.pbid);
    mFreeRbids.push_back(bullet.rbid);
    // Do this last, this invalidates `bullet`
    mBullets[bullet.rbid].reset();
}

void Ionl::Document::ReparentBullet(Bullet& bullet, Bullet& newParent, int index) {
    // Update database
    mStore->SetBulletPosition(bullet.pbid, newParent.pbid, index);

    // Update in-memory objects
    if (auto oldParent = GetBulletByPbid(bullet.parentPbid)) {
        auto pos = std::find(oldParent->children.begin(), oldParent->children.end(), bullet.pbid);
        if (pos != oldParent->children.end()) {
            oldParent->children.erase(pos);
        }
    }
    bullet.parentPbid = newParent.pbid;
    {
        auto pos = newParent.children.begin() + index;
        newParent.children.insert(pos, bullet.pbid);
    }
}

Ionl::Bullet* Ionl::Document::Store(Bullet bullet) {
    Bullet* result;

    // Put into storage
    if (mFreeRbids.empty()) {
        mBullets.push_back(std::move(bullet));

        result = &mBullets.back().value();
        result->rbid = mBullets.size() - 1;
    } else {
        size_t rbid = bullet.rbid = mFreeRbids.back();
        mFreeRbids.pop_back();
        mBullets[rbid] = std::move(bullet);

        result = &mBullets[rbid].value();
    }

    // Update pbid->rbid mapping
    mPtoRmap.try_emplace(result->pbid, result->rbid);

    return result;
}