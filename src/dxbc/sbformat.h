// Microsoft's tokenized shader format definitions, usable on Windows and in host tests.
#pragma once
#ifdef _WIN32
#include <windows.h>
#else
typedef unsigned int UINT;
#endif
#include "../../third_party/microsoft/d3d12TokenizedProgramFormat.hpp"
