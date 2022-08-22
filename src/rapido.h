//STUB
#ifndef RAPIDO_H
#define RAPIDO_H

#include "net.h"

//Called once to initialize the Rapido library
void *rapido_init();

//Connect connection c to host
status rapido_connect(connection *c, char *host);
//Close connection c
status rapido_close(connection *c);
//File descriptor has some data available for conection c
//will write this data in c->buf and write the amount of bytes written in *sz
status rapido_read(connection *c, size_t *sz);
//File descriptor is ready to write sz bytes into buffer buf. The amount actually written is returned in *wrote 
status rapido_write(connection *c, char *buf, size_t sz, size_t *wrote);
//File descriptor is readable, should return the number of bytes actually available (e.g. for SSL calls SSL_Pending
size_t rapido_readable(connection *);

//rapido_writable could be written also if needed

#endif /* RAPIDO_H */
