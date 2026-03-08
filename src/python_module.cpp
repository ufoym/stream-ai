#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <memory>
#include <new>
#include <vector>

#include "streamcam/streamcam.h"

namespace {

PyObject* g_streamcam_error = nullptr;

struct PyStreamcamReader {
  PyObject_HEAD
  streamcam_reader* reader;
};

bool ReaderIsOpen(PyStreamcamReader* self) { return self->reader != nullptr; }

PyObject* SetStreamcamError(const char* context, streamcam_status status) {
  if (context == nullptr || context[0] == '\0') {
    PyErr_SetString(g_streamcam_error, streamcam_status_string(status));
  } else {
    PyErr_Format(g_streamcam_error, "%s: %s", context,
                 streamcam_status_string(status));
  }
  return nullptr;
}

PyObject* DeviceInfoToDict(const streamcam_device_info& device) {
  PyObject* result = PyDict_New();
  if (result == nullptr) {
    return nullptr;
  }

  PyObject* id = PyUnicode_FromString(device.id);
  PyObject* name = PyUnicode_FromString(device.name);
  if (id == nullptr || name == nullptr) {
    Py_XDECREF(id);
    Py_XDECREF(name);
    Py_DECREF(result);
    return nullptr;
  }

  const int set_id = PyDict_SetItemString(result, "id", id);
  const int set_name = PyDict_SetItemString(result, "name", name);
  Py_DECREF(id);
  Py_DECREF(name);
  if (set_id < 0 || set_name < 0) {
    Py_DECREF(result);
    return nullptr;
  }

  return result;
}

PyObject* FrameToDict(const streamcam_frame_view& frame) {
  PyObject* result = PyDict_New();
  if (result == nullptr) {
    return nullptr;
  }

  PyObject* data = PyBytes_FromStringAndSize(
      reinterpret_cast<const char*>(frame.data),
      static_cast<Py_ssize_t>(frame.size_bytes));
  if (data == nullptr) {
    Py_DECREF(result);
    return nullptr;
  }

  const auto set_unsigned = [&](const char* key, unsigned long long value) {
    PyObject* number = PyLong_FromUnsignedLongLong(value);
    if (number == nullptr) {
      return -1;
    }
    const int rc = PyDict_SetItemString(result, key, number);
    Py_DECREF(number);
    return rc;
  };

  const auto set_signed = [&](const char* key, long value) {
    PyObject* number = PyLong_FromLong(value);
    if (number == nullptr) {
      return -1;
    }
    const int rc = PyDict_SetItemString(result, key, number);
    Py_DECREF(number);
    return rc;
  };

  if (PyDict_SetItemString(result, "data", data) < 0 ||
      set_unsigned("size_bytes", frame.size_bytes) < 0 ||
      set_unsigned("width", frame.width) < 0 ||
      set_unsigned("height", frame.height) < 0 ||
      set_unsigned("stride_bytes", frame.stride_bytes) < 0 ||
      set_unsigned("timestamp_ns", frame.timestamp_ns) < 0 ||
      set_unsigned("sequence", frame.sequence) < 0 ||
      set_signed("pixel_format", frame.pixel_format) < 0) {
    Py_DECREF(data);
    Py_DECREF(result);
    return nullptr;
  }

  Py_DECREF(data);
  return result;
}

void ReaderClose(PyStreamcamReader* self) {
  if (self->reader != nullptr) {
    streamcam_close(self->reader);
    self->reader = nullptr;
  }
}

void ReaderDealloc(PyStreamcamReader* self) {
  ReaderClose(self);
  Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

PyObject* ReaderStart(PyStreamcamReader* self, PyObject* /*args*/) {
  if (!ReaderIsOpen(self)) {
    PyErr_SetString(PyExc_RuntimeError, "reader is closed");
    return nullptr;
  }

  const streamcam_status status = streamcam_start(self->reader);
  if (status != STREAMCAM_STATUS_OK) {
    return SetStreamcamError("start", status);
  }

  Py_RETURN_NONE;
}

PyObject* ReaderStop(PyStreamcamReader* self, PyObject* /*args*/) {
  if (!ReaderIsOpen(self)) {
    PyErr_SetString(PyExc_RuntimeError, "reader is closed");
    return nullptr;
  }

  const streamcam_status status = streamcam_stop(self->reader);
  if (status != STREAMCAM_STATUS_OK) {
    return SetStreamcamError("stop", status);
  }

  Py_RETURN_NONE;
}

PyObject* ReaderCloseMethod(PyStreamcamReader* self, PyObject* /*args*/) {
  ReaderClose(self);
  Py_RETURN_NONE;
}

PyObject* ReaderGetLatestFrame(PyStreamcamReader* self, PyObject* /*args*/) {
  if (!ReaderIsOpen(self)) {
    PyErr_SetString(PyExc_RuntimeError, "reader is closed");
    return nullptr;
  }

  streamcam_frame_view frame{};
  const streamcam_status status =
      streamcam_get_latest_frame(self->reader, &frame);
  if (status == STREAMCAM_STATUS_NO_FRAME) {
    Py_RETURN_NONE;
  }
  if (status != STREAMCAM_STATUS_OK) {
    return SetStreamcamError("get_latest_frame", status);
  }

  return FrameToDict(frame);
}

PyObject* ReaderEnter(PyStreamcamReader* self, PyObject* /*args*/) {
  return Py_NewRef(reinterpret_cast<PyObject*>(self));
}

PyObject* ReaderExit(PyStreamcamReader* self, PyObject* args) {
  PyObject* exc_type = nullptr;
  PyObject* exc_value = nullptr;
  PyObject* traceback = nullptr;
  if (!PyArg_ParseTuple(args, "OOO", &exc_type, &exc_value, &traceback)) {
    return nullptr;
  }

  ReaderClose(self);
  Py_RETURN_FALSE;
}

PyObject* ReaderIsClosed(PyStreamcamReader* self, void* /*closure*/) {
  if (ReaderIsOpen(self)) {
    Py_RETURN_FALSE;
  }
  Py_RETURN_TRUE;
}

PyGetSetDef kReaderGetSet[] = {
    {"closed", reinterpret_cast<getter>(ReaderIsClosed), nullptr,
     const_cast<char*>("True when the reader has been closed."), nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyMethodDef kReaderMethods[] = {
    {"start", reinterpret_cast<PyCFunction>(ReaderStart), METH_NOARGS,
     PyDoc_STR("Start camera capture.")},
    {"stop", reinterpret_cast<PyCFunction>(ReaderStop), METH_NOARGS,
     PyDoc_STR("Stop camera capture.")},
    {"close", reinterpret_cast<PyCFunction>(ReaderCloseMethod), METH_NOARGS,
     PyDoc_STR("Close the camera reader.")},
    {"get_latest_frame", reinterpret_cast<PyCFunction>(ReaderGetLatestFrame),
     METH_NOARGS, PyDoc_STR("Return the latest frame dict or None.")},
    {"__enter__", reinterpret_cast<PyCFunction>(ReaderEnter), METH_NOARGS,
     PyDoc_STR("Enter a context manager.")},
    {"__exit__", reinterpret_cast<PyCFunction>(ReaderExit), METH_VARARGS,
     PyDoc_STR("Exit a context manager and close the reader.")},
    {nullptr, nullptr, 0, nullptr},
};

PyTypeObject kReaderType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
};

PyObject* ModuleVersion(PyObject* /*self*/, PyObject* /*args*/) {
  return PyUnicode_FromString(streamcam_version());
}

PyObject* ModuleStatusString(PyObject* /*self*/, PyObject* args) {
  int status = 0;
  if (!PyArg_ParseTuple(args, "i", &status)) {
    return nullptr;
  }
  return PyUnicode_FromString(
      streamcam_status_string(static_cast<streamcam_status>(status)));
}

PyObject* ModuleDefaultConfig(PyObject* /*self*/, PyObject* /*args*/) {
  const streamcam_config config = streamcam_default_config();
  return Py_BuildValue(
      "{s:I,s:I,s:I,s:i}", "width", config.width, "height", config.height,
      "fps", config.fps, "preferred_format", config.preferred_format);
}

PyObject* ModuleListDevices(PyObject* /*self*/, PyObject* /*args*/) {
  size_t count = 0;
  streamcam_status status = streamcam_list_devices(nullptr, 0, &count);
  if (status != STREAMCAM_STATUS_OK) {
    return SetStreamcamError("list_devices", status);
  }

  PyObject* result = PyList_New(0);
  if (result == nullptr) {
    return nullptr;
  }

  if (count == 0) {
    return result;
  }

  std::vector<streamcam_device_info> devices(count);
  status = streamcam_list_devices(devices.data(), devices.size(), &count);
  if (status != STREAMCAM_STATUS_OK) {
    Py_DECREF(result);
    return SetStreamcamError("list_devices", status);
  }

  for (size_t i = 0; i < count; ++i) {
    PyObject* device = DeviceInfoToDict(devices[i]);
    if (device == nullptr) {
      Py_DECREF(result);
      return nullptr;
    }
    if (PyList_Append(result, device) < 0) {
      Py_DECREF(device);
      Py_DECREF(result);
      return nullptr;
    }
    Py_DECREF(device);
  }

  return result;
}

PyObject* ModuleOpen(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
  const char* device_id = nullptr;
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int fps = 0;
  int preferred_format = STREAMCAM_PIXEL_FORMAT_NATIVE;
  static const char* const kwlist[] = {"device_id", "width", "height", "fps",
                                       "preferred_format", nullptr};

  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "s|IIIi", const_cast<char**>(kwlist), &device_id, &width,
          &height, &fps, &preferred_format)) {
    return nullptr;
  }

  streamcam_config config = streamcam_default_config();
  if (width != 0) {
    config.width = width;
  }
  if (height != 0) {
    config.height = height;
  }
  if (fps != 0) {
    config.fps = fps;
  }
  config.preferred_format =
      static_cast<streamcam_pixel_format>(preferred_format);

  streamcam_reader* reader = nullptr;
  const streamcam_status status = streamcam_open(device_id, &config, &reader);
  if (status != STREAMCAM_STATUS_OK) {
    return SetStreamcamError("open", status);
  }

  PyStreamcamReader* result =
      PyObject_New(PyStreamcamReader, &kReaderType);
  if (result == nullptr) {
    streamcam_close(reader);
    return PyErr_NoMemory();
  }

  result->reader = reader;
  return reinterpret_cast<PyObject*>(result);
}

PyMethodDef kModuleMethods[] = {
    {"version", reinterpret_cast<PyCFunction>(ModuleVersion), METH_NOARGS,
     PyDoc_STR("Return the streamcam library version.")},
    {"status_string", reinterpret_cast<PyCFunction>(ModuleStatusString),
     METH_VARARGS, PyDoc_STR("Return a human-readable status string.")},
    {"default_config", reinterpret_cast<PyCFunction>(ModuleDefaultConfig),
     METH_NOARGS, PyDoc_STR("Return the default camera config dict.")},
    {"list_devices", reinterpret_cast<PyCFunction>(ModuleListDevices),
     METH_NOARGS, PyDoc_STR("Return a list of available camera devices.")},
    {"open", reinterpret_cast<PyCFunction>(ModuleOpen),
     METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR("Open a camera reader for a device id.")},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef kModuleDef = {
    PyModuleDef_HEAD_INIT,
    "streamcam",
    "CPython extension for the streamcam native library.",
    -1,
    kModuleMethods,
};

bool AddIntConstant(PyObject* module, const char* name, int value) {
  return PyModule_AddIntConstant(module, name, value) == 0;
}

}  // namespace

PyMODINIT_FUNC PyInit_streamcam(void) {
  kReaderType.tp_name = "streamcam.Reader";
  kReaderType.tp_basicsize = sizeof(PyStreamcamReader);
  kReaderType.tp_dealloc =
      reinterpret_cast<destructor>(ReaderDealloc);
  kReaderType.tp_flags = Py_TPFLAGS_DEFAULT;
  kReaderType.tp_doc = PyDoc_STR("Active streamcam reader.");
  kReaderType.tp_methods = kReaderMethods;
  kReaderType.tp_getset = kReaderGetSet;

  if (PyType_Ready(&kReaderType) < 0) {
    return nullptr;
  }

  PyObject* module = PyModule_Create(&kModuleDef);
  if (module == nullptr) {
    return nullptr;
  }

  g_streamcam_error = PyErr_NewException("streamcam.Error", nullptr, nullptr);
  if (g_streamcam_error == nullptr) {
    Py_DECREF(module);
    return nullptr;
  }

  if (PyModule_AddObject(module, "Error", g_streamcam_error) < 0) {
    Py_DECREF(g_streamcam_error);
    Py_DECREF(module);
    return nullptr;
  }

  Py_INCREF(&kReaderType);
  if (PyModule_AddObject(module, "Reader",
                         reinterpret_cast<PyObject*>(&kReaderType)) < 0) {
    Py_DECREF(&kReaderType);
    Py_DECREF(module);
    return nullptr;
  }

  if (!AddIntConstant(module, "STATUS_OK", STREAMCAM_STATUS_OK) ||
      !AddIntConstant(module, "STATUS_INVALID_ARGUMENT",
                      STREAMCAM_STATUS_INVALID_ARGUMENT) ||
      !AddIntConstant(module, "STATUS_NOT_FOUND", STREAMCAM_STATUS_NOT_FOUND) ||
      !AddIntConstant(module, "STATUS_NOT_SUPPORTED",
                      STREAMCAM_STATUS_NOT_SUPPORTED) ||
      !AddIntConstant(module, "STATUS_PLATFORM_ERROR",
                      STREAMCAM_STATUS_PLATFORM_ERROR) ||
      !AddIntConstant(module, "STATUS_NO_FRAME", STREAMCAM_STATUS_NO_FRAME) ||
      !AddIntConstant(module, "PIXEL_FORMAT_UNKNOWN",
                      STREAMCAM_PIXEL_FORMAT_UNKNOWN) ||
      !AddIntConstant(module, "PIXEL_FORMAT_NATIVE",
                      STREAMCAM_PIXEL_FORMAT_NATIVE) ||
      !AddIntConstant(module, "PIXEL_FORMAT_BGRA32",
                      STREAMCAM_PIXEL_FORMAT_BGRA32) ||
      !AddIntConstant(module, "PIXEL_FORMAT_NV12",
                      STREAMCAM_PIXEL_FORMAT_NV12) ||
      !AddIntConstant(module, "PIXEL_FORMAT_MJPEG",
                      STREAMCAM_PIXEL_FORMAT_MJPEG) ||
      !AddIntConstant(module, "PIXEL_FORMAT_YUY2",
                      STREAMCAM_PIXEL_FORMAT_YUY2)) {
    Py_DECREF(module);
    return nullptr;
  }

  return module;
}
