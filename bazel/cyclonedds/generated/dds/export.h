/*
 * Stand-in for the CMake GenerateExportHeader output.
 * The Bazel build produces a static library, so no import/export
 * decorations are required.
 */
#ifndef DDS_EXPORT_H
#define DDS_EXPORT_H

#define DDS_EXPORT
#define DDS_NO_EXPORT
#define DDS_DEPRECATED
#define DDS_DEPRECATED_EXPORT
#define DDS_DEPRECATED_NO_EXPORT
#define DDS_INLINE_EXPORT

#endif
