BASE_ALLOCATORS = $(abspath ../../allocators)

PATH_ALLOCATORS = $(subst $(BASE_ALLOCATORS)/Makefile,  , $(wildcard $(BASE_ALLOCATORS)/*))
ALLOCATORS = $(filter-out Makefile nballoc.mk, $(subst $(BASE_ALLOCATORS)/,  ,  $(PATH_ALLOCATORS))) libc
INTERMEDIATE_OBJS_PATH = bin
MY_ALLOCATORS = 1lvl-nb 1lvl-sl 4lvl-nb

CC=gcc
CFLAGS= -I$(BASE_ALLOCATORS)/$* -I../../utils 
FLAGS= -O3 -g -Wall -I../../utils -MMD -MP -MF $*.d
LIBRARY = -lpthread

.SECONDARY:

all:  $(INTERMEDIATE_OBJS_PATH) $(addprefix $(TARGET)-, $(ALLOCATORS))

$(INTERMEDIATE_OBJS_PATH):
	mkdir $(INTERMEDIATE_OBJS_PATH)

$(TARGET)-libc: $(TARGET)-libc.o  
	@echo linking $* $(TARGET)-libc.o $(BASE_ALLOCATORS)/libc/nballoc.o ../../utils/utils.o
	$(CC) $(INTERMEDIATE_OBJS_PATH)/$(TARGET)-libc.o    -o $(TARGET)-libc $(LIBRARY) -L$(BASE_ALLOCATORS)/$*


$(TARGET)-%-nb: main.c
	@echo compiling for $@
	$(CC) $(FLAGS) main.c ../../allocators/$*-nb/nballoc.c ../../utils/utils.c  -I$(BASE_ALLOCATORS)/$*-nb -I../../utils   -o $(TARGET)-$*-nb -DALLOCATOR=$*-nb -D'TO_BE_REPLACED_MALLOC(x)=bd_xx_malloc(x)' -D'TO_BE_REPLACED_FREE(x)=bd_xx_free(x)' -lpthread -D'ALLOCATOR_NAME="$*-nb"'

$(TARGET)-%-sl: main.c
	@echo compiling for $@
	$(CC) $(FLAGS)  main.c ../../allocators/$*-sl/nballoc.c ../../utils/utils.c  -I$(BASE_ALLOCATORS)/$*-sl -I../../utils -o $(TARGET)-$*-sl -DALLOCATOR=$*-sl -D'TO_BE_REPLACED_MALLOC(x)=bd_xx_malloc(x)' -D'TO_BE_REPLACED_FREE(x)=bd_xx_free(x)' -lpthread -D'ALLOCATOR_NAME="$*-sl"'


$(TARGET)-%: $(TARGET)-%.o  #$(BASE_ALLOCATORS)/%/nballoc.o
	@echo linking $* $(TARGET)-$*.o $(BASE_ALLOCATORS)/$*/nballoc.o ../../utils/utils.o
	$(CC) $(INTERMEDIATE_OBJS_PATH)/$(TARGET)-$*.o  -L$(BASE_ALLOCATORS)/$* -l$* -o $(TARGET)-$* $(LIBRARY)

$(TARGET)-libc.o: main.c
	@echo compiling for $@
	$(CC) main.c $(CFLAGS) -c -o bin/$(TARGET)-libc.o -DALLOCATOR=libc -D'TO_BE_REPLACED_MALLOC(x)=malloc(x)' -D'TO_BE_REPLACED_FREE(x)=free(x)' -D'ALLOCATOR_NAME="libc"'


$(TARGET)-%.o: main.c 
	@echo compiling for $@
	$(CC) main.c $(CFLAGS) -c -o bin/$(TARGET)-$*.o -DALLOCATOR=$* -D'TO_BE_REPLACED_MALLOC(x)=malloc(x)' -D'TO_BE_REPLACED_FREE(x)=free(x)' -D'ALLOCATOR_NAME="$*"'



clean:
	-rm $(TARGET)-*
	-rm bin -R

.PHONY: clean
