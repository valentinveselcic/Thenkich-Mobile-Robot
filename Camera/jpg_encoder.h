// jpg_encoder.h
#include <stdlib.h>

bool rgb565_to_jpeg(uint8_t *rgb565, uint32_t width, uint32_t height, uint8_t **jpeg, size_t *jpeg_size) {
  // Simple placeholder - replace with actual JPEG encoder
  *jpeg = (uint8_t*)malloc(100);
  *jpeg_size = 100;
  return (*jpeg != NULL);
}