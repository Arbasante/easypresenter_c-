#include "main_ui.h" 
#include <sqlite3.h>
#include <vector>
#include <string>
#include <iostream>
#include <memory>
#include <filesystem>
#include <mutex>
#include <regex>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <thread>
#include <functional>

namespace fs = std::filesystem;

struct CantoDB { int id; std::string titulo; std::string tono; std::string categoria; };
struct Diapositiva { int id; int orden; std::string texto; };
struct LibroBiblia { int id; std::string nombre; int capitulos; };
struct Versiculo { int capitulo; int versiculo; std::string texto; };

struct CacheKey {
    int version_id; int libro_numero; int capitulo;
    bool operator==(const CacheKey& other) const { return version_id == other.version_id && libro_numero == other.libro_numero && capitulo == other.capitulo; }
};
struct CacheKeyHash { std::size_t operator()(const CacheKey& k) const { return std::hash<int>()(k.version_id) ^ (std::hash<int>()(k.libro_numero) << 1) ^ (std::hash<int>()(k.capitulo) << 2); } };

struct VersionInfo { int id; std::string sigla; std::string nombre_completo; int prioridad; };

const std::vector<std::string> NOMBRES_LIBROS = {
    "Génesis", "Éxodo", "Levítico", "Números", "Deuteronomio", "Josué", "Jueces", "Rut", "1 Samuel", "2 Samuel",
    "1 Reyes", "2 Reyes", "1 Crónicas", "2 Crónicas", "Esdras", "Nehemías", "Ester", "Job", "Salmos", "Proverbios",
    "Eclesiastés", "Cantares", "Isaías", "Jeremías", "Lamentaciones", "Ezequiel", "Daniel", "Oseas", "Joel", "Amós",
    "Abdías", "Jonás", "Miqueas", "Nahúm", "Habacuc", "Sofonías", "Hageo", "Zacarías", "Malaquías", "Mateo",
    "Marcos", "Lucas", "Juan", "Hechos", "Romanos", "1 Corintios", "2 Corintios", "Gálatas", "Efesios", "Filipenses",
    "Colosenses", "1 Tesalonicenses", "2 Tesalonicenses", "1 Timoteo", "2 Timoteo", "Tito", "Filemón", "Hebreos", "Santiago",
    "1 Pedro", "2 Pedro", "1 Juan", "2 Juan", "3 Juan", "Judas", "Apocalipsis"
};

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::string to_lower(const std::string& str) {
    std::string lower_str = str;
    std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), ::tolower);
    return lower_str;
}

std::string buscar_libro_inteligente(const std::string& query, int& out_id) {
    std::string q = to_lower(trim(query));
    if (q.empty()) return "";
    for (size_t i = 0; i < NOMBRES_LIBROS.size(); ++i) {
        if (to_lower(NOMBRES_LIBROS[i]).find(q) == 0) { out_id = i + 1; return NOMBRES_LIBROS[i]; }
    }
    return "";
}

class AppState {
private:
    sqlite3* cantos_db = nullptr;
    sqlite3* biblias_db = nullptr;
    std::mutex db_mutex;
    std::vector<VersionInfo> versiones_cargadas;
    int current_version_id = 1;
    std::unordered_map<CacheKey, std::vector<Versiculo>, CacheKeyHash> chapter_cache;
    std::mutex cache_mutex;

    sqlite3* setup_db(const std::string& db_name) {
        std::string base_path = "/home/basanteriano/Documentos/EasyPresenter/EasyPresenter_c++/data/";
        if (!fs::exists(base_path)) fs::create_directories(base_path);
        sqlite3* db;
        if (sqlite3_open((base_path + db_name).c_str(), &db) != SQLITE_OK) return nullptr;
        sqlite3_exec(db, "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
        return db;
    }

    void insert_diapositivas_intern(int canto_id, const std::string& letra) {
        size_t start = 0, end = 0; int orden = 1;
        while ((end = letra.find("\n\n", start)) != std::string::npos) {
            std::string estrofa = trim(letra.substr(start, end - start));
            if (!estrofa.empty()) {
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(cantos_db, "INSERT INTO diapositivas (canto_id, orden, texto) VALUES (?, ?, ?)", -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(stmt, 1, canto_id); sqlite3_bind_int(stmt, 2, orden++); sqlite3_bind_text(stmt, 3, estrofa.c_str(), -1, SQLITE_TRANSIENT); sqlite3_step(stmt);
                } sqlite3_finalize(stmt);
            } start = end + 2;
        }
        std::string estrofa = trim(letra.substr(start));
        if (!estrofa.empty()) {
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(cantos_db, "INSERT INTO diapositivas (canto_id, orden, texto) VALUES (?, ?, ?)", -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, canto_id); sqlite3_bind_int(stmt, 2, orden); sqlite3_bind_text(stmt, 3, estrofa.c_str(), -1, SQLITE_TRANSIENT); sqlite3_step(stmt);
            } sqlite3_finalize(stmt);
        }
    }

    void procesar_versiones() {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(biblias_db, "SELECT id, nombre FROM versiones", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int id = sqlite3_column_int(stmt, 0); 
                std::string nombre_bd = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                std::string n_lower = to_lower(nombre_bd);
                
                std::string sigla = "OTRA"; 
                std::string nombre_comp = nombre_bd;
                int prioridad = 999; 

                // Lógica de Alias (Siglas) y Nombres Completos Oficiales
                if (n_lower.find("reina") != std::string::npos || n_lower.find("1960") != std::string::npos) { 
                    sigla = "RVR"; nombre_comp = "Reina Valera 1960"; prioridad = 0; 
                }
                else if (n_lower.find("nvi") != std::string::npos) { 
                    sigla = "NVI"; nombre_comp = "Nueva Versión Internacional"; prioridad = 1; 
                }
                else if (n_lower.find("ntv") != std::string::npos) { 
                    sigla = "NTV"; nombre_comp = "Nueva Traducción Viviente"; prioridad = 2; 
                }
                else if (n_lower.find("lbla") != std::string::npos || n_lower.find("americas") != std::string::npos) { 
                    sigla = "LBLA"; nombre_comp = "La Biblia de las Américas"; prioridad = 3; 
                }
                else if (n_lower.find("tla") != std::string::npos) { 
                    sigla = "TLA"; nombre_comp = "Traducción en Lenguaje Actual"; prioridad = 4; 
                }
                else if (n_lower.find("dhh") != std::string::npos || n_lower.find("interconfesional") != std::string::npos) { 
                    sigla = "DHH"; nombre_comp = "Dios Habla Hoy"; prioridad = 5; 
                }
                else {
                    // Si metes una biblia nueva rara, agarra las primeras 4 letras en mayúscula
                    sigla = nombre_bd.substr(0, 4);
                    std::transform(sigla.begin(), sigla.end(), sigla.begin(), ::toupper);
                }

                versiones_cargadas.push_back({id, sigla, nombre_comp, prioridad});
            }
        } sqlite3_finalize(stmt);
        
        std::sort(versiones_cargadas.begin(), versiones_cargadas.end(), [](const VersionInfo& a, const VersionInfo& b) { return a.prioridad < b.prioridad; });
        if (!versiones_cargadas.empty()) current_version_id = versiones_cargadas[0].id;
    }

public:
    AppState() { cantos_db = setup_db("cantos.db"); biblias_db = setup_db("biblias.db"); if(biblias_db) procesar_versiones(); }
    ~AppState() { if (cantos_db) sqlite3_close(cantos_db); if (biblias_db) sqlite3_close(biblias_db); }

    std::vector<CantoDB> get_all_cantos() {
        std::lock_guard<std::mutex> lock(db_mutex); std::vector<CantoDB> lista; sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(cantos_db, "SELECT id, titulo FROM cantos ORDER BY titulo", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) lista.push_back({ sqlite3_column_int(stmt, 0), reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)), "", "" });
        } sqlite3_finalize(stmt); return lista;
    }
    std::vector<CantoDB> get_cantos_filtrados(const std::string& busqueda) {
        std::lock_guard<std::mutex> lock(db_mutex); std::vector<CantoDB> lista; sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(cantos_db, "SELECT id, titulo FROM cantos WHERE titulo LIKE ? ORDER BY titulo", -1, &stmt, nullptr) == SQLITE_OK) {
            std::string parametro = "%" + busqueda + "%"; sqlite3_bind_text(stmt, 1, parametro.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) lista.push_back({ sqlite3_column_int(stmt, 0), reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)), "", "" });
        } sqlite3_finalize(stmt); return lista;
    }
    std::string get_canto_titulo(int id) {
        std::lock_guard<std::mutex> lock(db_mutex); std::string titulo = ""; sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(cantos_db, "SELECT titulo FROM cantos WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, id); if (sqlite3_step(stmt) == SQLITE_ROW) titulo = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        } sqlite3_finalize(stmt); return titulo;
    }
    std::vector<Diapositiva> get_canto_diapositivas(int canto_id) {
        std::lock_guard<std::mutex> lock(db_mutex); std::vector<Diapositiva> lista; sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(cantos_db, "SELECT id, orden, texto FROM diapositivas WHERE canto_id = ? ORDER BY orden", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, canto_id);
            while (sqlite3_step(stmt) == SQLITE_ROW) lista.push_back({ sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1), reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)) });
        } sqlite3_finalize(stmt); return lista;
    }
    void add_canto(const std::string& titulo, const std::string& letra) {
        std::lock_guard<std::mutex> lock(db_mutex); sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(cantos_db, "INSERT INTO cantos (titulo, tono, categoria) VALUES (?, '', 'Personalizado')", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, titulo.c_str(), -1, SQLITE_TRANSIENT); sqlite3_step(stmt);
        } sqlite3_finalize(stmt); insert_diapositivas_intern(sqlite3_last_insert_rowid(cantos_db), letra);
    }
    void update_canto(int id, const std::string& titulo, const std::string& letra) {
        std::lock_guard<std::mutex> lock(db_mutex); sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(cantos_db, "UPDATE cantos SET titulo = ? WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, titulo.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int(stmt, 2, id); sqlite3_step(stmt);
        } sqlite3_finalize(stmt);
        if (sqlite3_prepare_v2(cantos_db, "DELETE FROM diapositivas WHERE canto_id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, id); sqlite3_step(stmt);
        } sqlite3_finalize(stmt); insert_diapositivas_intern(id, letra);
    }
    void delete_canto(int id) {
        std::lock_guard<std::mutex> lock(db_mutex); sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(cantos_db, "DELETE FROM diapositivas WHERE canto_id = ?", -1, &stmt, nullptr) == SQLITE_OK) { sqlite3_bind_int(stmt, 1, id); sqlite3_step(stmt); } sqlite3_finalize(stmt);
        if (sqlite3_prepare_v2(cantos_db, "DELETE FROM cantos WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK) { sqlite3_bind_int(stmt, 1, id); sqlite3_step(stmt); } sqlite3_finalize(stmt);
    }

    std::vector<VersionInfo> get_versiones() { return versiones_cargadas; }

    // NUEVO: Busca por el Nombre Completo
    std::string set_version_by_name(const std::string& name) {
        std::lock_guard<std::mutex> lock(db_mutex);
        for(const auto& v : versiones_cargadas) {
            if(v.nombre_completo == name) { current_version_id = v.id; return v.nombre_completo; }
        } return "";
    }

    std::vector<LibroBiblia> get_libros_biblia() {
        std::lock_guard<std::mutex> lock(db_mutex); std::vector<LibroBiblia> lista; if (!biblias_db) return lista; sqlite3_stmt* stmt;
        const char* sql = "SELECT libro_numero, libro_nombre, MAX(capitulo) FROM versiculos WHERE version_id = ? GROUP BY libro_numero ORDER BY libro_numero";
        if (sqlite3_prepare_v2(biblias_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, current_version_id);
            while (sqlite3_step(stmt) == SQLITE_ROW) lista.push_back({ sqlite3_column_int(stmt, 0), reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)), sqlite3_column_int(stmt, 2) });
        } sqlite3_finalize(stmt); return lista;
    }

    void get_capitulo_async(int libro_numero, int capitulo, std::function<void(std::vector<Versiculo>)> callback) {
        int v_id = current_version_id; 
        std::thread([this, v_id, libro_numero, capitulo, callback]() {
            std::vector<Versiculo> lista;
            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                CacheKey key{v_id, libro_numero, capitulo};
                if (chapter_cache.find(key) != chapter_cache.end()) {
                    lista = chapter_cache[key];
                    slint::invoke_from_event_loop([callback, lista]() { callback(lista); }); return;
                }
            }
            {
                std::lock_guard<std::mutex> lock(db_mutex); sqlite3_stmt* stmt;
                const char* sql = "SELECT versiculo, texto FROM versiculos WHERE version_id = ? AND libro_numero = ? AND capitulo = ? ORDER BY versiculo";
                if (sqlite3_prepare_v2(biblias_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(stmt, 1, v_id); sqlite3_bind_int(stmt, 2, libro_numero); sqlite3_bind_int(stmt, 3, capitulo);
                    while (sqlite3_step(stmt) == SQLITE_ROW) lista.push_back({ capitulo, sqlite3_column_int(stmt, 0), reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) });
                } sqlite3_finalize(stmt);
            }
            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                CacheKey key{v_id, libro_numero, capitulo}; chapter_cache[key] = lista;
            }
            slint::invoke_from_event_loop([callback, lista]() { callback(lista); });
        }).detach(); 
    }
};

int main() {
    AppState app_state;
    auto ui = AppWindow::create();
    auto proyector = ProjectorWindow::create();

    auto current_biblia_libro = std::make_shared<int>(-1);
    auto current_biblia_capitulo = std::make_shared<int>(-1);

    auto cargar_cantos = [&app_state, ui](const std::string& busqueda) {
        auto cantos_db = busqueda.empty() ? app_state.get_all_cantos() : app_state.get_cantos_filtrados(busqueda);
        std::vector<Canto> cantos_slint;
        if (cantos_db.empty()) cantos_slint.push_back(Canto{ 0, slint::SharedString("Click derecho para agregar canto"), slint::SharedString("") });
        else for(const auto& c : cantos_db) cantos_slint.push_back(Canto{ c.id, slint::SharedString(c.titulo), slint::SharedString("") });
        ui->set_cantos(std::make_shared<slint::VectorModel<Canto>>(cantos_slint));
    };
    cargar_cantos("");

    ui->on_seleccionar_canto([ui, &app_state, current_biblia_libro, current_biblia_capitulo](int id) {
        if (id == 0) return; 
        *current_biblia_libro = -1; *current_biblia_capitulo = -1;
        ui->set_elemento_seleccionado(slint::SharedString(app_state.get_canto_titulo(id)));
        auto diapositivas = app_state.get_canto_diapositivas(id); std::vector<DiapositivaUI> diapos_slint;
        for(const auto& dia : diapositivas) diapos_slint.push_back(DiapositivaUI{ slint::SharedString(std::to_string(dia.orden)), slint::SharedString(dia.texto) });
        ui->set_estrofas_actuales(std::make_shared<slint::VectorModel<DiapositivaUI>>(diapos_slint));
        ui->set_active_estrofa_index(-1); ui->invoke_focus_panel();
    });

    ui->on_abrir_proyector([proyector]() mutable { proyector->window().set_position(slint::PhysicalPosition({1920, 0})); proyector->window().set_fullscreen(true); proyector->show(); });
    
    ui->on_proyectar_estrofa([proyector](slint::SharedString texto, slint::SharedString referencia) mutable { 
        proyector->set_texto_proyeccion(texto); 
        proyector->set_referencia(referencia); 
    });

    auto versiones = app_state.get_versiones();
    std::vector<slint::SharedString> versiones_slint;
    // AQUÍ PASAMOS LOS NOMBRES COMPLETOS AL COMBOBOX
    for (const auto& v : versiones) versiones_slint.push_back(slint::SharedString(v.nombre_completo));
    ui->set_bible_versions(std::make_shared<slint::VectorModel<slint::SharedString>>(versiones_slint));
    
    if (!versiones.empty()) { 
        ui->set_current_bible_version(slint::SharedString(versiones[0].nombre_completo)); 
        app_state.set_version_by_name(versiones[0].nombre_completo); 
    }

    auto cargar_libros_biblia = [&app_state, ui]() {
        auto libros = app_state.get_libros_biblia(); std::vector<BookInfo> libros_slint;
        for(const auto& lib : libros) libros_slint.push_back(BookInfo{ lib.id, slint::SharedString(lib.nombre), lib.capitulos });
        ui->set_bible_books(std::make_shared<slint::VectorModel<BookInfo>>(libros_slint));
    };
    cargar_libros_biblia();

    // AQUÍ EL CALLBACK RECIBE EL NOMBRE COMPLETO ("Reina Valera 1960") DESDE EL COMBOBOX
    ui->on_bible_version_changed([&app_state, ui, proyector, cargar_libros_biblia, current_biblia_libro, current_biblia_capitulo](slint::SharedString name) mutable {
        app_state.set_version_by_name(std::string(name));
        cargar_libros_biblia();

        if (*current_biblia_libro != -1 && *current_biblia_capitulo != -1) {
            int active_idx = ui->get_active_estrofa_index();
            app_state.get_capitulo_async(*current_biblia_libro, *current_biblia_capitulo, [ui, proyector, active_idx](std::vector<Versiculo> versiculos) {
                std::vector<DiapositivaUI> diapos_slint;
                for(const auto& v : versiculos) diapos_slint.push_back(DiapositivaUI{ slint::SharedString(std::to_string(v.versiculo)), slint::SharedString(v.texto) });
                ui->set_estrofas_actuales(std::make_shared<slint::VectorModel<DiapositivaUI>>(diapos_slint));
                if (active_idx >= 0 && active_idx < versiculos.size()) proyector->set_texto_proyeccion(slint::SharedString(versiculos[active_idx].texto));
            });
        }
    });

    ui->on_bible_search_changed([ui](slint::SharedString query) {
        std::string q = std::string(query); int fake_id; std::string sug = buscar_libro_inteligente(q, fake_id);
        if(!sug.empty() && sug != q) ui->set_bible_search_suggestion(slint::SharedString(sug));
        else ui->set_bible_search_suggestion("");
    });

    ui->on_bible_search_accepted([&app_state, ui, current_biblia_libro, current_biblia_capitulo](slint::SharedString query) {
        std::string q = std::string(query);
        std::regex re(R"(^\s*(.*?)\s+(\d+)(?:\s+(\d+))?\s*$)"); 
        std::smatch match;

        if (std::regex_match(q, match, re)) {
            std::string book_query = match[1].str();
            int capitulo = std::stoi(match[2].str());
            int versiculo_objetivo = match[3].matched ? std::stoi(match[3].str()) : 1;

            int libro_id = -1;
            std::string libro_nombre_real = buscar_libro_inteligente(book_query, libro_id);

            if (libro_id != -1) {
                *current_biblia_libro = libro_id;
                *current_biblia_capitulo = capitulo;
                std::string titulo = libro_nombre_real + " " + std::to_string(capitulo);
                
                ui->set_elemento_seleccionado(slint::SharedString(titulo));

                app_state.get_capitulo_async(libro_id, capitulo, [ui, versiculo_objetivo, titulo](std::vector<Versiculo> versiculos) {
                    std::vector<DiapositivaUI> diapos_slint;
                    
                    for(const auto& v : versiculos) {
                        if (v.versiculo >= versiculo_objetivo) {
                            diapos_slint.push_back(DiapositivaUI{ 
                                slint::SharedString(std::to_string(v.versiculo)), 
                                slint::SharedString(v.texto) 
                            });
                        }
                    }
                    
                    ui->set_estrofas_actuales(std::make_shared<slint::VectorModel<DiapositivaUI>>(diapos_slint));
                    ui->set_active_estrofa_index(0); 
                    
                    if (!diapos_slint.empty()) {
                        ui->invoke_proyectar_estrofa(diapos_slint[0].texto, slint::SharedString(titulo + ":" + std::string(diapos_slint[0].orden)));
                    }
                    ui->invoke_focus_panel();
                });
                ui->set_bible_search_suggestion("");
            }
        }
    });

    ui->on_bible_book_selected([ui](BookInfo book) {
        std::vector<ChapterRow> filas; std::vector<int> fila_actual;
        for (int i = 1; i <= book.capitulos; ++i) {
            fila_actual.push_back(i);
            if (fila_actual.size() == 5 || i == book.capitulos) {
                auto fila_slint = std::make_shared<slint::VectorModel<int>>();
                for(int cap : fila_actual) fila_slint->push_back(cap);
                filas.push_back(ChapterRow{ fila_slint }); fila_actual.clear();
            }
        } ui->set_chapter_rows(std::make_shared<slint::VectorModel<ChapterRow>>(filas));
    });

    ui->on_bible_chapter_selected([ui, &app_state, current_biblia_libro, current_biblia_capitulo](int cap) {
        BookInfo book = ui->get_selected_bible_book();
        *current_biblia_libro = book.id; *current_biblia_capitulo = cap;
        std::string titulo = std::string(book.nombre) + " " + std::to_string(cap);
        
        app_state.get_capitulo_async(book.id, cap, [ui, titulo](std::vector<Versiculo> versiculos) {
            ui->set_elemento_seleccionado(slint::SharedString(titulo)); 
            std::vector<DiapositivaUI> diapos_slint;
            for(const auto& v : versiculos) diapos_slint.push_back(DiapositivaUI{ slint::SharedString(std::to_string(v.versiculo)), slint::SharedString(v.texto) });
            ui->set_estrofas_actuales(std::make_shared<slint::VectorModel<DiapositivaUI>>(diapos_slint));
            ui->set_active_estrofa_index(0);
            if (!versiculos.empty()) ui->invoke_proyectar_estrofa(slint::SharedString(versiculos[0].texto), slint::SharedString(titulo + ":" + std::to_string(versiculos[0].versiculo)));
            ui->invoke_focus_panel();
        });
    });

    ui->on_buscar_cantos([cargar_cantos](slint::SharedString texto) { cargar_cantos(std::string(texto)); });

    ui->on_cargar_datos_edicion([ui, &app_state](int id) {
        std::string titulo = app_state.get_canto_titulo(id); auto diapositivas = app_state.get_canto_diapositivas(id);
        std::string letra_completa = ""; for (size_t i = 0; i < diapositivas.size(); ++i) { letra_completa += diapositivas[i].texto; if (i < diapositivas.size() - 1) letra_completa += "\n\n"; }
        ui->set_form_id(id); ui->set_form_titulo(slint::SharedString(titulo)); ui->set_form_letra(slint::SharedString(letra_completa)); ui->set_mostrar_formulario(true);
    });

    ui->on_guardar_canto([&app_state, cargar_cantos](int id, slint::SharedString titulo, slint::SharedString letra) {
        if (id == -1) app_state.add_canto(std::string(titulo), std::string(letra)); else app_state.update_canto(id, std::string(titulo), std::string(letra)); cargar_cantos("");
    });

    ui->on_eliminar_canto([&app_state, cargar_cantos](int id) { app_state.delete_canto(id); cargar_cantos(""); });

    ui->run();
    return 0;
}