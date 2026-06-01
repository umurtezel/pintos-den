#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

/* ELF Yapıları */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

struct Elf32_Ehdr {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
};

struct Elf32_Phdr {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_STACK   0x6474e551
#define PF_X 1
#define PF_W 2
#define PF_R 4

static thread_func start_process;
static bool load (const char *file_name, void (**eip) (void), void **esp);
static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes, bool writable);
static bool install_page (void *upage, void *kpage, bool writable);

tid_t process_execute (const char *file_name) 
{
  char *fn_copy;
  char *fn_copy2;
  tid_t tid;

  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL) return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  fn_copy2 = palloc_get_page (0);
  if (fn_copy2 == NULL) { palloc_free_page (fn_copy); return TID_ERROR; }
  strlcpy (fn_copy2, file_name, PGSIZE);

  char *save_ptr;
  char *thread_name = strtok_r (fn_copy2, " ", &save_ptr);
  tid = thread_create (thread_name, PRI_DEFAULT, start_process, fn_copy);
  palloc_free_page (fn_copy2);

  if (tid == TID_ERROR)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  /* Ebeveyn, çocuğun load işlemini bitirmesini bekler */
  struct child_status *child = NULL;
  struct list_elem *e;
  for (e = list_begin (&thread_current()->child_list); e != list_end (&thread_current()->child_list); e = list_next (e)) 
    {
      struct child_status *cs = list_entry (e, struct child_status, elem);
      if (cs->tid == tid) { child = cs; break; }
    }

  if (child != NULL) 
    {
      sema_down (&child->load_sema); /* Yükleme bitene kadar bekle */
      if (!child->load_success) return -1; /* Yükleme çöktüyse -1 dön */
    }

  return tid;
}

static void start_process (void *file_name_) 
{
  char *file_name = file_name_; 
  struct intr_frame if_;
  bool success;

  /* Ismi bozmamak için bir kopyasını alıyoruz*/
  char *fn_copy = palloc_get_page(0);
  if (fn_copy == NULL)
    {
      palloc_free_page (file_name);
      thread_exit();
    }
  strlcpy(fn_copy, file_name, PGSIZE);

  char *save_ptr;
  char *executable = strtok_r (fn_copy, " ", &save_ptr);

  struct thread *t = thread_current();
  t->pagedir = NULL;

  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  success = load (executable, &if_.eip, &if_.esp);

  if (t->child_info != NULL) 
    {
      t->child_info->load_success = success;
      sema_up (&t->child_info->load_sema);
    }
  
  if (success) 
    {
      /* --- ARGUMENT PASSING (YIĞIN KURULUMU) --- */
      int argc = 0;
      char *argv[128];
      char *token;
      char *save_ptr2;
      
      /* 1. Gelen tam komutu boşluklardan ayır */
      for (token = strtok_r (file_name, " ", &save_ptr2); token != NULL; token = strtok_r (NULL, " ", &save_ptr2)) 
        {
          argv[argc++] = token;
        }
        
      uint32_t argv_addresses[128];
      
      /* 2. String karakterlerini yığına (stack) tersten ekle */
      for (int i = argc - 1; i >= 0; i--) 
        {
          size_t len = strlen(argv[i]) + 1;
          if_.esp -= len;
          memcpy (if_.esp, argv[i], len);
          argv_addresses[i] = (uint32_t) if_.esp;
        }
        
      /* 3. Word-align (4 byte hızalama boşluğu) */
      while ((uint32_t) if_.esp % 4 != 0) 
        {
          if_.esp--;
          *(uint8_t *) if_.esp = 0;
        }
        
      /* 4. argv[argc] = NULL pointer'ı */
      if_.esp -= 4;
      *(uint32_t *) if_.esp = 0;
      
      /* 5. argv pointer'larının adreslerini tersten ekle */
      for (int i = argc - 1; i >= 0; i--) 
        {
          if_.esp -= 4;
          *(uint32_t *) if_.esp = argv_addresses[i];
        }
        
      /* 6. argv'nin kendi başlangıç adresi (char **) */
      uint32_t argv_start = (uint32_t) if_.esp;
      if_.esp -= 4;
          *(uint32_t *) if_.esp = argv_start;
      
      /* 7. argc değeri */
      if_.esp -= 4;
      *(int *) if_.esp = argc;
      
      /* 8. Sahte Return Address (C kütüphanesi için 0) */
      if_.esp -= 4;
      *(uint32_t *) if_.esp = 0;
    }

  palloc_free_page (fn_copy);
  palloc_free_page (file_name);

  if (!success) 
    thread_exit ();

  /* Kullanıcı programına (User-Space) geçiş! */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED (); 
}

int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  struct child_status *child = NULL;

  /* Çocuğu kendi listemizde arıyoruz */
  for (e = list_begin (&cur->child_list); e != list_end (&cur->child_list); e = list_next (e))
    {
      struct child_status *cs = list_entry (e, struct child_status, elem);
      if (cs->tid == child_tid)
        {
          child = cs;
          break;
        }
    }

  /* Eğer böyle bir çocuk yoksa veya daha önce wait edildiyse -1 dön (Kural ihlali) */
  if (child == NULL || child->was_waited)
    return -1;

  /* Çocuğun 1 kez beklendiğini işaretle */
  child->was_waited = true;

  /* EĞER ÇOCUK HENÜZ KAPANMADIYSA, SEMA_DOWN İLE UYU! */
  if (!child->is_exited)
    {
      sema_down (&child->wait_sema);
    }

  /* Uyandık! Çocuk kapandı, çıkış kodunu al ve hafıza sızıntısı olmasın diye sil */
  int status = child->exit_status;
  list_remove (&child->elem);
  free (child);

  return status;
}

void process_exit (void) {
  struct thread *cur = thread_current ();

  struct list_elem *e;
  while (!list_empty (&cur->child_list)) 
    {
      e = list_pop_front (&cur->child_list);
      struct child_status *cs = list_entry (e, struct child_status, elem);
      free (cs); /* Yetim kalan hafızayı (memory leak) temizle */
    }

  /* --- EBEVEYNİ UYANDIRMA MEKANİZMASI --- */
  if (cur->child_info != NULL)
    {
      cur->child_info->is_exited = true;
      sema_up (&cur->child_info->wait_sema); /* Uyuyan ebeveyne 'ben bittim' dürtmesi */
    }

    if (cur->exec_file != NULL)
    {
      file_allow_write (cur->exec_file);
      file_close (cur->exec_file);
    }

  uint32_t *pd = cur->pagedir;
  if (pd != NULL) {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

void process_activate (void) {
  struct thread *t = thread_current ();
  pagedir_activate (t->pagedir);
  tss_update ();
}

static bool load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;


  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) goto done;

  process_activate ();

  file = filesys_open (file_name);
  if (file == NULL) goto done; 

  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) {
      goto done; 
  }


  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
      struct Elf32_Phdr phdr;
      if (file_ofs < 0 || file_ofs > file_length (file)) goto done;
      file_seek (file, file_ofs);
      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr) goto done;
      file_ofs += sizeof phdr;
      
      if (phdr.p_type == PT_LOAD) {
          if (validate_segment (&phdr, file)) {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0) {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
              } else {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
              }
              if (!load_segment (file, file_page, (void *) mem_page, read_bytes, zero_bytes, writable)) {
                  goto done;
              }
          } else {
              goto done;
          }
      }
  }

  if (!setup_stack (esp)) {
      goto done;
  }
  
  *eip = (void (*) (void)) ehdr.e_entry;
  success = true;

done:

/* Başarılıysa dosyayı kilitle ve hafızada tut, başarısızsa kapat */
  if (success) 
    {
      file_deny_write (file);
      thread_current ()->exec_file = file;
    } 
  else 
    {
      file_close (file);
    }
  
  return success;
}

static bool validate_segment (const struct Elf32_Phdr *phdr, struct file *file) {
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) return false; 
  if (phdr->p_offset > (Elf32_Off) file_length (file)) return false;
  if (phdr->p_memsz < phdr->p_filesz) return false; 
  if (phdr->p_memsz == 0) return false;
  if (!is_user_vaddr ((void *) phdr->p_vaddr)) return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz))) return false;
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr) return false;
  if (phdr->p_vaddr < PGSIZE) return false;
  return true;
}

static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL) return false;
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
          palloc_free_page (kpage);
          return false; 
      }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);
      if (!install_page (upage, kpage, writable)) {
          palloc_free_page (kpage);
          return false; 
      }
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

static bool setup_stack (void **esp) 
{
  uint8_t *kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      bool success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE; 
      else
        palloc_free_page (kpage);
      return success;
    }
  return false;
}

static bool install_page (void *upage, void *kpage, bool writable) {
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}