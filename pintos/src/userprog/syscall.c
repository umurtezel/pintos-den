#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/synch.h"
#include "userprog/process.h"

struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);
void sys_exit (int status);
void check_ptr (const void *vaddr);
void check_buffer (const void *buffer, unsigned size);
void check_string (const char *str);

void syscall_init (void) {
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

void sys_exit (int status) {
  struct thread *cur = thread_current ();
  for (int i = 2; i < 128; i++) {
    if (cur->fd_table[i] != NULL) {
      file_close (cur->fd_table[i]);
      cur->fd_table[i] = NULL;
    }
  }
  if (cur->child_info != NULL)
    cur->child_info->exit_status = status;
  printf ("%s: exit(%d)\n", cur->name, status);
  thread_exit ();
}

void check_ptr (const void *vaddr) {
  if (vaddr == NULL || !is_user_vaddr (vaddr) || pagedir_get_page (thread_current ()->pagedir, vaddr) == NULL)
    sys_exit (-1);
}

void check_buffer (const void *buffer, unsigned size) {
  for (unsigned i = 0; i < size; i++)
    check_ptr ((const uint8_t *) buffer + i);
}

void check_string (const char *str) {
  check_ptr (str);
  while (*str != '\0') {
    str++;
    check_ptr (str);
  }
}

static void syscall_handler (struct intr_frame *f) {
  uint32_t *esp = (uint32_t *) f->esp;
  check_buffer (esp, 4);
  int syscall_number = esp[0];
  struct thread *cur = thread_current ();

  if (syscall_number == SYS_HALT) {
      shutdown_power_off ();
  }
  else if (syscall_number == SYS_EXIT) {
      check_buffer (esp + 1, 4);
      sys_exit ((int) esp[1]);
  }
  else if (syscall_number == SYS_EXEC) {
      check_buffer (esp + 1, 4);
      check_string ((const char *) esp[1]);
      lock_acquire (&filesys_lock);
      f->eax = process_execute ((const char *) esp[1]);
      lock_release (&filesys_lock);
  }
  else if (syscall_number == SYS_WAIT) {
      check_buffer (esp + 1, 4);
      f->eax = process_wait ((tid_t) esp[1]);
  }
  else if (syscall_number == SYS_CREATE) {
      check_buffer (esp + 1, 4); check_buffer (esp + 2, 4);
      check_string ((const char *) esp[1]);
      lock_acquire (&filesys_lock);
      f->eax = filesys_create ((const char *) esp[1], (unsigned) esp[2]);
      lock_release (&filesys_lock);
  }
  else if (syscall_number == SYS_REMOVE) {
      check_buffer (esp + 1, 4);
      check_string ((const char *) esp[1]);
      lock_acquire (&filesys_lock);
      f->eax = filesys_remove ((const char *) esp[1]);
      lock_release (&filesys_lock);
  }
  else if (syscall_number == SYS_OPEN) {
      check_buffer (esp + 1, 4);
      check_string ((const char *) esp[1]);
      lock_acquire (&filesys_lock);
      struct file *file_obj = filesys_open ((const char *) esp[1]);
      lock_release (&filesys_lock);
      
      if (file_obj == NULL) {
          f->eax = -1;
      } else {
          if (cur->fd_last < 128) {
              cur->fd_table[cur->fd_last] = file_obj;
              f->eax = cur->fd_last;
              cur->fd_last++;
          } else {
              f->eax = -1;
          }
      }
  }
  else if (syscall_number == SYS_FILESIZE) {
      check_buffer (esp + 1, 4);
      int fd = (int) esp[1];
      if (fd >= 2 && fd < 128 && cur->fd_table[fd] != NULL) {
          lock_acquire (&filesys_lock);
          f->eax = file_length (cur->fd_table[fd]);
          lock_release (&filesys_lock);
      } else {
          f->eax = -1;
      }
  }
  else if (syscall_number == SYS_READ) {
      check_buffer (esp + 1, 4); check_buffer (esp + 2, 4); check_buffer (esp + 3, 4);
      int fd = (int) esp[1];
      void *buffer = (void *) esp[2];
      unsigned size = (unsigned) esp[3];
      check_buffer (buffer, size);

      if (fd == 0) {
          for (unsigned i = 0; i < size; i++)
              ((uint8_t *)buffer)[i] = input_getc();
          f->eax = size;
      } else if (fd >= 2 && fd < 128 && cur->fd_table[fd] != NULL) {
          lock_acquire (&filesys_lock);
          f->eax = file_read (cur->fd_table[fd], buffer, size);
          lock_release (&filesys_lock);
      } else {
          f->eax = -1;
      }
  }
  else if (syscall_number == SYS_WRITE) {
      check_buffer (esp + 1, 4); check_buffer (esp + 2, 4); check_buffer (esp + 3, 4);
      int fd = (int) esp[1];
      const void *buffer = (const void *) esp[2];
      unsigned size = (unsigned) esp[3];
      check_buffer (buffer, size);

      if (fd == 1) {
          putbuf (buffer, size);
          f->eax = size;
      } else if (fd >= 2 && fd < 128 && cur->fd_table[fd] != NULL) {
          lock_acquire (&filesys_lock);
          f->eax = file_write (cur->fd_table[fd], buffer, size);
          lock_release (&filesys_lock);
      } else {
          f->eax = 0;
      }
  }
  else if (syscall_number == SYS_SEEK) {
      check_buffer (esp + 1, 4); check_buffer (esp + 2, 4);
      int fd = (int) esp[1];
      unsigned position = (unsigned) esp[2];
      if (fd >= 2 && fd < 128 && cur->fd_table[fd] != NULL) {
          lock_acquire (&filesys_lock);
          file_seek (cur->fd_table[fd], position);
          lock_release (&filesys_lock);
      }
  }
  else if (syscall_number == SYS_TELL) {
      check_buffer (esp + 1, 4);
      int fd = (int) esp[1];
      if (fd >= 2 && fd < 128 && cur->fd_table[fd] != NULL) {
          lock_acquire (&filesys_lock);
          f->eax = file_tell (cur->fd_table[fd]);
          lock_release (&filesys_lock);
      } else {
          f->eax = -1;
      }
  }
  else if (syscall_number == SYS_CLOSE) {
      check_buffer (esp + 1, 4);
      int fd = (int) esp[1];
      if (fd >= 2 && fd < 128 && cur->fd_table[fd] != NULL) {
          lock_acquire (&filesys_lock);
          file_close (cur->fd_table[fd]);
          lock_release (&filesys_lock);
          cur->fd_table[fd] = NULL;
      }
  }
  else {
      sys_exit (-1);
  }
}