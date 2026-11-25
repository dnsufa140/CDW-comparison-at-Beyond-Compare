#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Console & UTF-8 / UTF-16 helpers (proper Cyrillic output in CMD, PowerShell,
// Windows Terminal, and redirected stdout/stderr pipes)
// -----------------------------------------------------------------------------
std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        out.data(), needed, nullptr, nullptr);
    return out;
}

void WriteLine(DWORD stdHandleId, const std::wstring& message) {
    std::wstring line = message + L"\n";
    HANDLE h = GetStdHandle(stdHandleId);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) {
            DWORD written = 0;
            WriteConsoleW(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            return;
        }
        std::string utf8 = WideToUtf8(line);
        DWORD written = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        return;
    }
    std::string utf8 = WideToUtf8(line);
    FILE* stream = (stdHandleId == STD_ERROR_HANDLE) ? stderr : stdout;
    std::fwrite(utf8.data(), 1, utf8.size(), stream);
}

void PrintOut(const std::wstring& msg) { WriteLine(STD_OUTPUT_HANDLE, msg); }
void PrintErr(const std::wstring& msg) { WriteLine(STD_ERROR_HANDLE, msg); }

std::wstring FormatHResult(HRESULT hr) {
    wchar_t hexBuf[16] = {};
    std::swprintf(hexBuf, 16, L"0x%08X", static_cast<unsigned int>(static_cast<uint32_t>(hr)));
    std::wstring out(hexBuf);

    LPWSTR buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(
        flags, nullptr, static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    if (len > 0 && buf != nullptr) {
        std::wstring desc(buf, len);
        LocalFree(buf);
        while (!desc.empty() && (desc.back() == L'\r' || desc.back() == L'\n' || desc.back() == L' ')) {
            desc.pop_back();
        }
        if (!desc.empty()) {
            out += L" (" + desc + L")";
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// Win32 Unicode path helpers (zero <filesystem> overhead)
// -----------------------------------------------------------------------------
std::wstring ExtractFileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

std::wstring ExtractParentDir(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return {};
    if (pos == 2 && path.size() >= 3 && path[1] == L':') {
        return path.substr(0, 3);  // Keep root slash like "C:\"
    }
    return path.substr(0, pos);
}

std::wstring ToAbsolutePath(const std::wstring& pathStr) {
    DWORD needed = GetFullPathNameW(pathStr.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return pathStr;
    std::wstring buf(needed, L'\0');
    DWORD len = GetFullPathNameW(pathStr.c_str(), needed, buf.data(), nullptr);
    if (len == 0 || len >= needed) return pathStr;
    buf.resize(len);
    return buf;
}

bool EnsureDirectoryExists(const std::wstring& dirPath) {
    if (dirPath.empty()) return true;
    DWORD attr = GetFileAttributesW(dirPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    // Create parent directory recursively before creating leaf directory
    std::wstring parent = ExtractParentDir(dirPath);
    if (!parent.empty() && parent != dirPath) {
        if (!EnsureDirectoryExists(parent)) {
            return false;
        }
    }

    if (CreateDirectoryW(dirPath.c_str(), nullptr)) {
        return true;
    }
    DWORD err = GetLastError();
    return err == ERROR_ALREADY_EXISTS;
}

// -----------------------------------------------------------------------------
// RAII wrappers for COM lifetime, IDispatch pointers, and VARIANT values
// -----------------------------------------------------------------------------
class ComInitGuard {
public:
    ComInitGuard() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComInitGuard() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    ComInitGuard(const ComInitGuard&) = delete;
    ComInitGuard& operator=(const ComInitGuard&) = delete;

    bool ok() const { return SUCCEEDED(hr_); }
    HRESULT hr() const { return hr_; }

private:
    HRESULT hr_;
};

class DispatchPtr {
public:
    DispatchPtr() = default;
    explicit DispatchPtr(IDispatch* p) : ptr_(p) {}
    ~DispatchPtr() { reset(); }

    DispatchPtr(const DispatchPtr&) = delete;
    DispatchPtr& operator=(const DispatchPtr&) = delete;

    DispatchPtr(DispatchPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    DispatchPtr& operator=(DispatchPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    void reset(IDispatch* p = nullptr) {
        if (ptr_ != nullptr) {
            ptr_->Release();
        }
        ptr_ = p;
    }

    IDispatch* get() const { return ptr_; }
    IDispatch** put() {
        reset();
        return &ptr_;
    }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    IDispatch* ptr_ = nullptr;
};

class VariantGuard {
public:
    VariantGuard() { VariantInit(&v_); }
    ~VariantGuard() { VariantClear(&v_); }

    VariantGuard(const VariantGuard&) = delete;
    VariantGuard& operator=(const VariantGuard&) = delete;

    VariantGuard(VariantGuard&& other) noexcept {
        v_ = other.v_;
        VariantInit(&other.v_);
    }

    const VARIANT& get() const { return v_; }
    VARIANT& ref() { return v_; }

    static VariantGuard FromInt(long val) {
        VariantGuard vg;
        vg.v_.vt = VT_I4;
        vg.v_.lVal = val;
        return vg;
    }

    static VariantGuard FromBool(bool val) {
        VariantGuard vg;
        vg.v_.vt = VT_BOOL;
        vg.v_.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
        return vg;
    }

    static VariantGuard FromString(const std::wstring& s) {
        VariantGuard vg;
        vg.v_.vt = VT_BSTR;
        vg.v_.bstrVal = SysAllocStringLen(s.data(), static_cast<UINT>(s.size()));
        if (vg.v_.bstrVal == nullptr) {
            throw std::runtime_error("SysAllocStringLen failed (out of memory)");
        }
        return vg;
    }

    static VariantGuard FromDispatch(IDispatch* disp) {
        VariantGuard vg;
        vg.v_.vt = VT_DISPATCH;
        vg.v_.pdispVal = disp;
        if (disp != nullptr) {
            disp->AddRef();
        }
        return vg;
    }

private:
    VARIANT v_{};
};

class ComError : public std::exception {
public:
    explicit ComError(std::wstring msg)
        : wmsg_(std::move(msg)), utf8_(WideToUtf8(wmsg_)) {}

    const std::wstring& wwhat() const noexcept { return wmsg_; }
    const char* what() const noexcept override { return utf8_.c_str(); }

private:
    std::wstring wmsg_;
    std::string utf8_;
};

// -----------------------------------------------------------------------------
// Late-binding IDispatch automation helpers (equivalent to win32com.client.dynamic)
// -----------------------------------------------------------------------------
DispatchPtr CreateDispatchByProgId(const wchar_t* progId) {
    CLSID clsid{};
    HRESULT hr = CLSIDFromProgID(progId, &clsid);
    if (FAILED(hr)) {
        throw ComError(std::wstring(L"ProgID не найден (") + progId + L"): " + FormatHResult(hr));
    }

    DispatchPtr disp;
    hr = CoCreateInstance(
        clsid, nullptr, CLSCTX_SERVER, IID_IDispatch,
        reinterpret_cast<void**>(disp.put()));
    if (FAILED(hr) || !disp) {
        throw ComError(std::wstring(L"Не удалось создать COM-объект ") + progId + L": " + FormatHResult(hr));
    }
    return disp;
}

DISPID GetDispId(IDispatch* disp, const wchar_t* name) {
    if (disp == nullptr) {
        throw ComError(std::wstring(L"Нулевой IDispatch при обращении к '") + name + L"'");
    }
    DISPID dispid = DISPID_UNKNOWN;
    LPOLESTR names[1] = { const_cast<LPOLESTR>(name) };
    HRESULT hr = disp->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &dispid);
    if (FAILED(hr)) {
        throw ComError(std::wstring(L"Метод/свойство '") + name + L"' не найдено: " + FormatHResult(hr));
    }
    return dispid;
}

std::wstring FormatInvokeFailure(const wchar_t* memberName, HRESULT hr, EXCEPINFO& excep) {
    std::wstring out = L"Ошибка вызова '" + std::wstring(memberName) + L"': " + FormatHResult(hr);
    if (hr == DISP_E_EXCEPTION) {
        if (excep.bstrDescription != nullptr && excep.bstrDescription[0] != L'\0') {
            out += L" — " + std::wstring(excep.bstrDescription);
        }
        if (excep.bstrSource != nullptr && excep.bstrSource[0] != L'\0') {
            out += L" [" + std::wstring(excep.bstrSource) + L"]";
        }
        if (excep.scode != 0) {
            out += L" (SCODE " + FormatHResult(excep.scode) + L")";
        }
    }
    if (excep.bstrSource) SysFreeString(excep.bstrSource);
    if (excep.bstrDescription) SysFreeString(excep.bstrDescription);
    if (excep.bstrHelpFile) SysFreeString(excep.bstrHelpFile);
    return out;
}

void PutProperty(IDispatch* disp, const wchar_t* name, VariantGuard value) {
    DISPID dispid = GetDispId(disp, name);
    DISPID dispidNamed = DISPID_PROPERTYPUT;
    DISPPARAMS params{};
    params.cArgs = 1;
    params.rgvarg = &value.ref();
    params.cNamedArgs = 1;
    params.rgdispidNamedArgs = &dispidNamed;

    EXCEPINFO excep{};
    UINT argErr = 0;
    HRESULT hr = disp->Invoke(
        dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT,
        &params, nullptr, &excep, &argErr);
    if (FAILED(hr)) {
        throw ComError(FormatInvokeFailure(name, hr, excep));
    }
}

VariantGuard InvokeMember(
    IDispatch* disp,
    const wchar_t* name,
    WORD flags,
    std::vector<VariantGuard> argsInCallOrder = {}) {

    DISPID dispid = GetDispId(disp, name);

    // COM IDispatch::Invoke requires rgvarg in REVERSE argument order.
    std::vector<VARIANT> reversedArgs;
    reversedArgs.reserve(argsInCallOrder.size());
    for (auto it = argsInCallOrder.rbegin(); it != argsInCallOrder.rend(); ++it) {
        reversedArgs.push_back(it->ref());
    }

    DISPPARAMS params{};
    params.cArgs = static_cast<UINT>(reversedArgs.size());
    params.rgvarg = reversedArgs.empty() ? nullptr : reversedArgs.data();

    VariantGuard result;
    EXCEPINFO excep{};
    UINT argErr = 0;
    HRESULT hr = disp->Invoke(
        dispid, IID_NULL, LOCALE_USER_DEFAULT, flags,
        &params, &result.ref(), &excep, &argErr);
    if (FAILED(hr)) {
        throw ComError(FormatInvokeFailure(name, hr, excep));
    }
    return result;
}

DispatchPtr ExtractDispatch(const VariantGuard& vg) {
    const VARIANT& v = vg.get();
    if (v.vt == VT_DISPATCH && v.pdispVal != nullptr) {
        v.pdispVal->AddRef();
        return DispatchPtr(v.pdispVal);
    }
    if (v.vt == (VT_DISPATCH | VT_BYREF) && v.ppdispVal != nullptr && *v.ppdispVal != nullptr) {
        (*v.ppdispVal)->AddRef();
        return DispatchPtr(*v.ppdispVal);
    }
    return DispatchPtr{};
}

bool VariantToBool(const VariantGuard& vg) {
    const VARIANT& v = vg.get();
    switch (v.vt) {
        case VT_BOOL: return v.boolVal != VARIANT_FALSE;
        case VT_I4:   return v.lVal != 0;
        case VT_I2:   return v.iVal != 0;
        case VT_UI4:  return v.ulVal != 0;
        case VT_INT:  return v.intVal != 0;
        default:      return false;
    }
}

// Ensures app7.Quit() is always called on scope exit, matching Python finally:
class KompasQuitGuard {
public:
    explicit KompasQuitGuard(DispatchPtr& app7) : app7_(app7) {}
    ~KompasQuitGuard() {
        if (app7_) {
            try {
                InvokeMember(app7_.get(), L"Quit", DISPATCH_METHOD);
            } catch (...) {
                // Suppress cleanup errors, matching Python's except Exception: pass
            }
        }
    }
    KompasQuitGuard(const KompasQuitGuard&) = delete;
    KompasQuitGuard& operator=(const KompasQuitGuard&) = delete;

private:
    DispatchPtr& app7_;
};

// -----------------------------------------------------------------------------
// CLI options parsing (1:1 parity with argparse in main.py)
// -----------------------------------------------------------------------------
struct ConvertOptions {
    std::wstring inputPath;
    std::wstring outputPath;
    int dpi = 300;
    int bpp = 24;
    bool grayscale = false;
    bool multipage = true;
};

void PrintUsage(const std::wstring& exeName) {
    PrintOut(
        L"Конвертер чертежей КОМПАС-3D (.cdw) в .png (C++ Native COM)\n\n"
        L"Использование:\n"
        L"  " + exeName + L" <input.cdw> <output.png> [опции]\n\n"
        L"Позиционные аргументы:\n"
        L"  input           Путь к исходному файлу .cdw\n"
        L"  output          Путь для сохранения выходного файла .png\n\n"
        L"Опции:\n"
        L"  --dpi <число>   Разрешение в DPI (по умолчанию: 300)\n"
        L"  --bpp <1|8|24>  Глубина цвета: 1, 8 или 24 (по умолчанию: 24)\n"
        L"  --gray          Сохранить в оттенках серого\n"
        L"  --single-page   Сохранить только первый лист\n"
        L"  -h, --help      Показать эту справку");
}

bool ParseStrictInt(const std::wstring& text, int& out) {
    if (text.empty()) return false;
    wchar_t* endPtr = nullptr;
    errno = 0;
    long long val = std::wcstoll(text.c_str(), &endPtr, 10);
    if (errno == ERANGE || endPtr == text.c_str() || *endPtr != L'\0') {
        return false;
    }
    if (val < std::numeric_limits<int>::min() || val > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(val);
    return true;
}

enum class ParseResult { Ok, ShowHelp, Error };

ParseResult ParseCommandLine(const std::vector<std::wstring>& args, ConvertOptions& opts) {
    std::wstring exeName = args.empty() ? L"cdw2png.exe" : ExtractFileName(args[0]);
    std::vector<std::wstring> positional;

    auto readOptionValue = [&](size_t& idx, const std::wstring& flag, bool hasInline, const std::wstring& inlineVal, std::wstring& outVal) -> bool {
        if (hasInline) {
            outVal = inlineVal;
            return true;
        }
        if (idx + 1 >= args.size()) {
            PrintErr(L"Ошибка аргументов: для опции " + flag + L" требуется значение.");
            return false;
        }
        outVal = args[++idx];
        return true;
    };

    for (size_t i = 1; i < args.size(); ++i) {
        const std::wstring& arg = args[i];
        if (arg == L"-h" || arg == L"--help") {
            PrintUsage(exeName);
            return ParseResult::ShowHelp;
        }
        if (arg == L"--gray") {
            opts.grayscale = true;
            continue;
        }
        if (arg == L"--single-page") {
            opts.multipage = false;
            continue;
        }
        if (arg == L"--dpi" || arg.rfind(L"--dpi=", 0) == 0) {
            bool hasInline = (arg.rfind(L"--dpi=", 0) == 0);
            std::wstring inlineVal = hasInline ? arg.substr(6) : L"";
            std::wstring raw;
            if (!readOptionValue(i, L"--dpi", hasInline, inlineVal, raw)) return ParseResult::Error;
            int dpiVal = 0;
            if (!ParseStrictInt(raw, dpiVal) || dpiVal <= 0) {
                PrintErr(L"Ошибка аргументов: некорректное значение --dpi '" + raw + L"' (ожидается целое число > 0).");
                return ParseResult::Error;
            }
            opts.dpi = dpiVal;
            continue;
        }
        if (arg == L"--bpp" || arg.rfind(L"--bpp=", 0) == 0) {
            bool hasInline = (arg.rfind(L"--bpp=", 0) == 0);
            std::wstring inlineVal = hasInline ? arg.substr(6) : L"";
            std::wstring raw;
            if (!readOptionValue(i, L"--bpp", hasInline, inlineVal, raw)) return ParseResult::Error;
            int bppVal = 0;
            if (!ParseStrictInt(raw, bppVal) || (bppVal != 1 && bppVal != 8 && bppVal != 24)) {
                PrintErr(L"Ошибка аргументов: недопустимое значение --bpp '" + raw + L"' (допустимо: 1, 8 или 24).");
                return ParseResult::Error;
            }
            opts.bpp = bppVal;
            continue;
        }
        if (!arg.empty() && arg[0] == L'-') {
            PrintErr(L"Ошибка аргументов: неизвестная опция '" + arg + L"'. Используйте --help.");
            return ParseResult::Error;
        }
        positional.push_back(arg);
    }

    if (positional.size() != 2) {
        PrintErr(L"Ошибка аргументов: необходимо указать <input.cdw> и <output.png>. Используйте --help.");
        return ParseResult::Error;
    }

    opts.inputPath = positional[0];
    opts.outputPath = positional[1];
    return ParseResult::Ok;
}

// -----------------------------------------------------------------------------
// Core conversion routine (1:1 port of convert_single_file from main.py)
// -----------------------------------------------------------------------------
bool ConvertSingleFile(const ConvertOptions& opts) {
    const std::wstring inputAbs = ToAbsolutePath(opts.inputPath);
    const std::wstring outputAbs = ToAbsolutePath(opts.outputPath);

    DWORD attr = GetFileAttributesW(inputAbs.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        PrintErr(L"Ошибка: входной файл не найден -> " + inputAbs);
        return false;
    }

    const std::wstring outputDir = ExtractParentDir(outputAbs);
    if (!outputDir.empty() && !EnsureDirectoryExists(outputDir)) {
        PrintErr(L"Ошибка: не удалось создать выходную директорию -> " + outputDir);
        return false;
    }

    ComInitGuard com;
    if (!com.ok()) {
        PrintErr(L"Критическая ошибка COM: не удалось инициализировать COM: " + FormatHResult(com.hr()));
        return false;
    }

    DispatchPtr app7;
    try {
        app7 = CreateDispatchByProgId(L"Kompas.Application.7");
        KompasQuitGuard quitGuard(app7);

        DispatchPtr kompas5 = CreateDispatchByProgId(L"Kompas.Application.5");

        PutProperty(app7.get(), L"Visible", VariantGuard::FromBool(false));
        PutProperty(app7.get(), L"HideMessage", VariantGuard::FromInt(1));

        VariantGuard docsVar = InvokeMember(
            app7.get(), L"Documents", DISPATCH_PROPERTYGET | DISPATCH_METHOD);
        DispatchPtr documents = ExtractDispatch(docsVar);
        if (!documents) {
            PrintErr(L"Ошибка: не удалось получить коллекцию Documents в КОМПАС.");
            return false;
        }

        std::vector<VariantGuard> openArgs;
        openArgs.push_back(VariantGuard::FromString(inputAbs));
        openArgs.push_back(VariantGuard::FromBool(false));  // Visible = False
        openArgs.push_back(VariantGuard::FromBool(true));   // ReadOnly = True

        VariantGuard doc7Var = InvokeMember(
            documents.get(), L"Open", DISPATCH_METHOD, std::move(openArgs));
        DispatchPtr doc7 = ExtractDispatch(doc7Var);
        if (!doc7) {
            PrintErr(L"Ошибка: не удалось открыть документ в КОМПАС -> " + inputAbs);
            return false;
        }

        auto closeDoc7 = [&]() {
            try {
                std::vector<VariantGuard> closeArgs;
                closeArgs.push_back(VariantGuard::FromInt(0));  // kdDoNotSaveChanges
                InvokeMember(doc7.get(), L"Close", DISPATCH_METHOD, std::move(closeArgs));
            } catch (...) {
            }
        };

        VariantGuard doc2dVar = InvokeMember(
            kompas5.get(), L"ActiveDocument2D", DISPATCH_PROPERTYGET | DISPATCH_METHOD);
        DispatchPtr doc2d = ExtractDispatch(doc2dVar);
        if (!doc2d) {
            PrintErr(L"Ошибка: файл не является 2D-чертежом -> " + inputAbs);
            closeDoc7();
            return false;
        }

        VariantGuard rasterVar = InvokeMember(
            doc2d.get(), L"RasterFormatParam", DISPATCH_METHOD | DISPATCH_PROPERTYGET);
        DispatchPtr rasterParam = ExtractDispatch(rasterVar);
        if (!rasterParam) {
            PrintErr(L"Ошибка: не удалось получить RasterFormatParam.");
            closeDoc7();
            return false;
        }

        InvokeMember(rasterParam.get(), L"Init", DISPATCH_METHOD);
        PutProperty(rasterParam.get(), L"format", VariantGuard::FromInt(3));            // 3 = PNG
        PutProperty(rasterParam.get(), L"extResolution", VariantGuard::FromInt(opts.dpi));
        PutProperty(rasterParam.get(), L"colorBPP", VariantGuard::FromInt(opts.bpp));  // 24 = TrueColor
        PutProperty(rasterParam.get(), L"colorType", VariantGuard::FromInt(3));        // 3 = COLOROBJECT
        PutProperty(rasterParam.get(), L"greyScale", VariantGuard::FromBool(opts.grayscale));
        PutProperty(rasterParam.get(), L"multiPageOutput", VariantGuard::FromBool(opts.multipage));
        PutProperty(rasterParam.get(), L"onlyThinLine", VariantGuard::FromBool(false));

        std::vector<VariantGuard> saveArgs;
        saveArgs.push_back(VariantGuard::FromString(outputAbs));
        saveArgs.push_back(VariantGuard::FromDispatch(rasterParam.get()));

        VariantGuard saveRes = InvokeMember(
            doc2d.get(), L"SaveAsToRasterFormat", DISPATCH_METHOD, std::move(saveArgs));
        bool saved = VariantToBool(saveRes);

        // Release dependent COM interfaces before closing document and quitting KOMPAS
        rasterParam.reset();
        doc2d.reset();
        closeDoc7();
        doc7.reset();
        documents.reset();
        kompas5.reset();

        if (saved) {
            PrintOut(L"Успешно: " + outputAbs);
            return true;
        } else {
            PrintErr(L"Ошибка сохранения в растр: " + outputAbs);
            return false;
        }
    } catch (const ComError& e) {
        PrintErr(L"Критическая ошибка COM: " + e.wwhat());
        return false;
    } catch (const std::exception& e) {
        std::string msg = e.what();
        std::wstring wmsg(msg.begin(), msg.end());
        PrintErr(L"Критическая ошибка: " + wmsg);
        return false;
    }
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr || argc <= 0) {
        PrintErr(L"Ошибка: не удалось разобрать аргументы командной строки.");
        return 2;
    }

    std::vector<std::wstring> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    LocalFree(argv);

    ConvertOptions opts;
    ParseResult parsed = ParseCommandLine(args, opts);
    if (parsed == ParseResult::ShowHelp) {
        return 0;
    }
    if (parsed == ParseResult::Error) {
        return 2;
    }

    return ConvertSingleFile(opts) ? 0 : 1;
}
