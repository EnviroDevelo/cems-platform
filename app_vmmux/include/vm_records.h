/* vm_records.h - the sequence / schedule records (vm_records.c). */
#ifndef VM_RECORDS_H
#define VM_RECORDS_H
#include <stdint.h>
#include <stddef.h>

/* The application check the scan calls before applying a record write (core design, 20 Oct).
 * May renumber the rows of the image (the platform's own copy). 0 = accept; otherwise nothing changes and why holds the reason for the operator. */
int vm_check_record(uint16_t id, const void *new_image, char *why, size_t cap);

#endif
