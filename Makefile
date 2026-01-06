CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -MMD -MP -DBM_THREADSAFE
LDFLAGS = -pthread
OBJS    = dberror.o buffer_mgr_stat.o buffer_mgr.o storage_mgr.o
TEST1   = test_assign2_1
TEST2   = test_assign2_2

all: $(TEST1).exe $(TEST2).exe

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST1).exe: $(OBJS) test_assign2_1.o
	$(CC) -o $@ $^ $(LDFLAGS)

$(TEST2).exe: $(OBJS) test_assign2_2.o
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TEST1) $(TEST2) $(TEST1).exe $(TEST2).exe *.o *.d *.bin

-include *.d
