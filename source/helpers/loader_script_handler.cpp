#include "helpers/loader_script_handler.hpp"

#include "views/view.hpp"
#include "helpers/utils.hpp"
#include "providers/provider.hpp"

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>

#include <cstring>
#include <filesystem>

using namespace std::literals::string_literals;

namespace hex {

    PyObject* LoaderScript::Py_getFilePath(PyObject *self, PyObject *args) {
        return PyUnicode_FromString(LoaderScript::s_filePath.c_str());
    }

    PyObject* LoaderScript::Py_addPatch(PyObject *self, PyObject *args) {
        u64 address;
        u8 *patches;
        Py_ssize_t count;

        if (!PyArg_ParseTuple(args, "K|y#", &address, &patches, &count)) {
            PyErr_BadArgument();
            return nullptr;
        }

        if (patches == nullptr || count == 0) {
            PyErr_SetString(PyExc_TypeError, "Invalid patch provided");
            return nullptr;
        }

        if (address >= LoaderScript::s_dataProvider->getActualSize()) {
            PyErr_SetString(PyExc_IndexError, "address out of range");
            return nullptr;
        }

        LoaderScript::s_dataProvider->write(address, patches, count);

        Py_RETURN_NONE;
    }

    PyObject* LoaderScript::Py_addBookmark(PyObject *self, PyObject *args) {
        Bookmark bookmark;

        char *name = nullptr;
        char *comment = nullptr;

        if (!PyArg_ParseTuple(args, "K|n|s|s", &bookmark.region.address, &bookmark.region.size, &name, &comment)) {
            PyErr_BadArgument();
            return nullptr;
        }

        if (name == nullptr || comment == nullptr) {
            PyErr_SetString(PyExc_IndexError, "address out of range");
            return nullptr;
        }

        std::copy(name, name + std::strlen(name), std::back_inserter(bookmark.name));
        std::copy(comment, comment + std::strlen(comment), std::back_inserter(bookmark.comment));

        View::postEvent(Events::AddBookmark, &bookmark);

        Py_RETURN_NONE;
    }

    static PyObject* createStructureType(std::string keyword, PyObject *args) {
        auto type = PyTuple_GetItem(args, 0);
        if (type == nullptr) {
            PyErr_BadArgument();
            return nullptr;
        }

        auto instance = PyObject_CallObject(type, nullptr);
        if (instance == nullptr) {
            PyErr_BadArgument();
            return nullptr;
        }

        hex::ScopeExit instanceCleanup([&]{ Py_DECREF(instance); });

        if (instance->ob_type->tp_base == nullptr || instance->ob_type->tp_base->tp_name != "ImHexType"s) {
            PyErr_SetString(PyExc_TypeError, "class type must extend from ImHexType");
            return nullptr;
        }

        auto dict = instance->ob_type->tp_dict;
        if (dict == nullptr) {
            PyErr_BadArgument();
            return nullptr;
        }

        auto annotations = PyDict_GetItemString(dict, "__annotations__");
        if (annotations == nullptr) {
            PyErr_BadArgument();
            return nullptr;
        }

        auto list = PyDict_Items(annotations);
        if (list == nullptr) {
            PyErr_BadArgument();
            return nullptr;
        }

        hex::ScopeExit listCleanup([&]{ Py_DECREF(list); });

        std::string code = keyword + " " + instance->ob_type->tp_name + " {\n";

        for (u16 i = 0; i < PyList_Size(list); i++) {
            auto item = PyList_GetItem(list, i);

            auto memberName = PyUnicode_AsUTF8(PyTuple_GetItem(item, 0));
            auto memberType = PyTuple_GetItem(item, 1);
            if (memberType == nullptr) {
                PyErr_SetString(PyExc_TypeError, "member needs to have a annotation extending from ImHexType");
                return nullptr;
            }
            auto memberTypeInstance = PyObject_CallObject(memberType, nullptr);
            if (memberTypeInstance == nullptr || memberTypeInstance->ob_type->tp_base == nullptr || memberTypeInstance->ob_type->tp_base->tp_name != "ImHexType"s) {
                PyErr_SetString(PyExc_TypeError, "member needs to have a annotation extending from ImHexType");
                Py_DECREF(memberTypeInstance);

                return nullptr;
            }

            code += "   "s + memberTypeInstance->ob_type->tp_name + " "s + memberName + ";\n";

            Py_DECREF(memberTypeInstance);
        }

        code += "};\n";

        View::postEvent(Events::AppendPatternLanguageCode, code.c_str());

        Py_RETURN_NONE;
    }

    PyObject* LoaderScript::Py_addStruct(PyObject *self, PyObject *args) {
        return createStructureType("struct", args);
    }

    PyObject* LoaderScript::Py_addUnion(PyObject *self, PyObject *args) {
        return createStructureType("union", args);
    }

    bool LoaderScript::processFile(std::string_view scriptPath) {
        Py_SetProgramName(Py_DecodeLocale(__argv[0], nullptr));

        if (std::filesystem::exists(std::filesystem::path(__argv[0]).parent_path().string() + "/lib/python3.8"))
            Py_SetPythonHome(Py_DecodeLocale(std::filesystem::path(__argv[0]).parent_path().string().c_str(), nullptr));

        PyImport_AppendInittab("_imhex", []() -> PyObject* {

            static PyMethodDef ImHexMethods[] = {
                { "get_file_path",  &LoaderScript::Py_getFilePath,  METH_NOARGS,  "Returns the path of the file being loaded."  },
                { "patch",          &LoaderScript::Py_addPatch,     METH_VARARGS, "Patches a region of memory"                  },
                { "add_bookmark",   &LoaderScript::Py_addBookmark,  METH_VARARGS, "Adds a bookmark"                             },
                { "add_struct",     &LoaderScript::Py_addStruct,    METH_VARARGS, "Adds a struct"                               },
                { "add_union",      &LoaderScript::Py_addUnion,     METH_VARARGS, "Adds a union"                                },
                { nullptr,          nullptr,               0,     nullptr                                       }
            };

            static PyModuleDef ImHexModule = {
                PyModuleDef_HEAD_INIT, "imhex", nullptr, -1, ImHexMethods, nullptr, nullptr, nullptr, nullptr
            };

            auto module =  PyModule_Create(&ImHexModule);
            if (module == nullptr)
                return nullptr;

            return module;
        });

        Py_Initialize();

        {
            auto sysPath = PySys_GetObject("path");
            auto path = PyUnicode_FromString("lib");

            PyList_Insert(sysPath, 0, path);
        }

        FILE *scriptFile = fopen(scriptPath.data(), "r");
        PyRun_SimpleFile(scriptFile, scriptPath.data());

        fclose(scriptFile);

        Py_Finalize();

        return true;
    }

}