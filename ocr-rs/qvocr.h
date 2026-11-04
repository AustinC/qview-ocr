#ifndef QVOCR_H
#define QVOCR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run OCR on RGBA8888 pixels. Returns JSON; free with qvocr_free_string. Blocking. */
char *qvocr_recognize_rgba(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride);
void qvocr_free_string(char *s);

#ifdef __cplusplus
}
#endif

#endif
