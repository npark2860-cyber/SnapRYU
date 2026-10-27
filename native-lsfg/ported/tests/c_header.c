#include "lsfg_cemu.h"
#include <string.h>
int main(void) {
  LsfgCemu *module = NULL;
  LsfgCemuError error = {0};
  if (lsfg_cemu_create(NULL, &module, &error) != LSFG_CEMU_INVALID_ARGUMENT)
    return 1;
  if (module || error.status != LSFG_CEMU_INVALID_ARGUMENT ||
      error.vk_result_valid)
    return 2;
  if (strcmp(lsfg_cemu_status_string(LSFG_CEMU_TIMEOUT), "TIMEOUT") != 0)
    return 3;
  if (lsfg_cemu_destroy(&module, &error) != LSFG_CEMU_OK)
    return 4;
  return 0;
}
