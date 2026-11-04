#ifndef GEX_SAVE_MENU_H
#define GEX_SAVE_MENU_H

#include <stdint.h>

int gex_save_init(const char *settings_path, void (*pump)(void));
void gex_save_shutdown(void);
/* 0 cancels, 1 starts the selected deck, 2 opens retail manual entry. */
int gex_save_choose(void);
int gex_save_menu_open(void);
int gex_save_modal_open(void);
int gex_save_key(int down, uint32_t key, uint32_t scan);
int gex_save_take_close_request(void);
void gex_save_checkpoint(uint32_t password);

#endif
