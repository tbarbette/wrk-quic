#ifndef UNITS_H
#define UNITS_H

char *format_binary(long double, int raw);
char *format_metric(long double, int raw);
char *format_time_us(long double, int raw);
char *format_time_s(long double, int raw);

int scan_metric(char *, uint64_t *);
int scan_time(char *, uint64_t *);

#endif /* UNITS_H */
