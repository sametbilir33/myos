#ifndef MYOS_SERIAL_H
#define MYOS_SERIAL_H

void serial_init(void);
void serial_write_char(char c);
void serial_write(const char *text);
void serial_write_hex(unsigned long long value);
void serial_write_dec(unsigned long long value);

#endif