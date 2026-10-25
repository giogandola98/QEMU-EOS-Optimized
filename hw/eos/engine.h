void engine_init(void);

/* raise the completion interrupt of EDMAC channel `channel` in `delay` interrupt timer steps */
void edmac_trigger_interrupt(int channel, int delay);
