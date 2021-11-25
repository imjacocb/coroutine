#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if __APPLE__ && __MACH__
	#include <sys/ucontext.h>
#else 
	#include <ucontext.h>
#endif 

// 超过了 MMAP_THRESHOLD（128 KB），将使用 mmap() 创建匿名映射
#define STACK_SIZE (1024*1024) // 1MB
#define DEFAULT_COROUTINE 16

struct coroutine;

struct schedule {
	char stack[STACK_SIZE];
	ucontext_t main;// 发生调度的上下文
	int nco;  // coroutine 的数量
	int cap;// 可支持的 coroutine 数量
	int running;  // 当前正在执行的 coroutine id
	struct coroutine **co; // 当前 schedule 下的 coroutine 列表，每个 coroutine 以 id 标识
};

struct coroutine {
	coroutine_func func; // coroutine 将要指向的函数
	void *ud; // 指向用户数据的指针
	ucontext_t ctx;
	struct schedule * sch; // 全局的 schedule 对象
	ptrdiff_t cap; // coroutine 栈的大小
	ptrdiff_t size; // coroutine 栈的当前大小
	int status; // 当前的状态
	char *stack; // coroutine 保存 S->stack 中内容的栈
};

struct coroutine * 
_co_new(struct schedule *S , coroutine_func func, void *ud) {
	struct coroutine * co = malloc(sizeof(*co));
	co->func = func;
	co->ud = ud;
	co->sch = S;
	co->cap = 0;
	co->size = 0;
	co->status = COROUTINE_READY;
	co->stack = NULL;
	return co;
}

void
_co_delete(struct coroutine *co) {
	free(co->stack);  // 释放 coroutine 自己分配的 stack（C->stack)
	free(co);// 释放 coroutine 对象
}

// 使用 coroutine 库时的初始化语句就是创建一个 schedule
struct schedule * 
coroutine_open(void) {
	struct schedule *S = malloc(sizeof(*S));
	S->nco = 0;
	S->cap = DEFAULT_COROUTINE; // 设置为 16
	S->running = -1;

     // 一个有 cap 个大小的 coroutine 指针数组
	S->co = malloc(sizeof(struct coroutine *) * S->cap);
	memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
	return S;
}

void 
coroutine_close(struct schedule *S) {
	int i;
	for (i=0;i<S->cap;i++) {
		struct coroutine * co = S->co[i];
		if (co) {
			_co_delete(co);
		}
	}
	free(S->co);
	S->co = NULL;
	free(S);
}

int 
coroutine_new(struct schedule *S, coroutine_func func, void *ud) {
    // 创建一个 coroutine 对象
	struct coroutine *co = _co_new(S, func , ud);

    // 如果 schedule 中的 coroutine 对象数量已经超过限定值
    // 扩容 2 倍
	if (S->nco >= S->cap) {
		int id = S->cap;
        // 调用 realloc() 扩容 2 倍 S->cap
		S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
		memset(S->co + S->cap , 0 , sizeof(struct coroutine *) * S->cap);
		S->co[S->cap] = co;
		S->cap *= 2;
		++S->nco;
		return id;
	} else {
		int i;
        // 遍历 coroutine 列表，找到一个空闲位置
        // 实际应该不需要遍历
		for (i=0;i<S->cap;i++) {
			int id = (i+S->nco) % S->cap;
			if (S->co[id] == NULL) {
				S->co[id] = co;
				++S->nco;
				return id;
			}
		}
	}
	assert(0);
	return -1;
}

//mainfunc 是用来执行 coroutine 的函数
static void
mainfunc(uint32_t low32, uint32_t hi32) {
	uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
	struct schedule *S = (struct schedule *)ptr;
	int id = S->running;
	struct coroutine *C = S->co[id];
	C->func(S,C->ud);
	_co_delete(C);
	S->co[id] = NULL;
	--S->nco;
	S->running = -1;
}

void 
coroutine_resume(struct schedule * S, int id) {
     // 获取对应 id 的 coroutine 对象
	assert(S->running == -1);
	assert(id >=0 && id < S->cap);
	struct coroutine *C = S->co[id];
	if (C == NULL)
		return;
	int status = C->status;

     // 根据 coroutine 的状态做分支
	switch(status) {
        // 如果是从来没有执行过的 coroutine
	case COROUTINE_READY:
		getcontext(&C->ctx);
		C->ctx.uc_stack.ss_sp = S->stack; // 将一直使用这个栈
		C->ctx.uc_stack.ss_size = STACK_SIZE;
		C->ctx.uc_link = &S->main;// 回到主函数中
		S->running = id;
		C->status = COROUTINE_RUNNING;// 将状态标记为 运行中
		uintptr_t ptr = (uintptr_t)S;
        
        // 将 C->ctx 指向 mainfunc 函数，并把 schedule 指针地址传递过去
        // mainfunc 是用来执行 coroutine 的函数
		makecontext(&C->ctx, (void (*)(void)) mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr>>32));
		swapcontext(&S->main, &C->ctx);// 将内存保存在 S->main 中，切换到 C->ctx
		break;
	case COROUTINE_SUSPEND:
     // 如果是之前运行过的，就把 C->stack 的 C->size 内容复制到 S->stack + STACK_SIZE - C->size 上
		memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size);
		S->running = id;
		C->status = COROUTINE_RUNNING;
		swapcontext(&S->main, &C->ctx);
		break;
	default:
		assert(0);
	}
}


/*
每个 coroutine 运行时都共享使用 S->stack （即大小为 1MB），
当发生 yield 动作时，coroutine 会调用 _save_stack 将 S->stack 的内容 copy 到自己的 C->stack 上。
当下一次获取到 CPU 时（发生 resume 动作时），则将 C->stack 上的内容 memcpy 到 S->stack 上，然后开始执行（swapcontext()
*/
static void
_save_stack(struct coroutine *C, char *top) {
    // dummy 将在 S->stack 上进行分配
    // top 指向 S->stack 的栈顶
	char dummy = 0;
	assert(top - &dummy <= STACK_SIZE);

    // 如果 C->stack 不够放下 coroutine 在 S->stack 上的内容
    // 重新进行分配
	if (C->cap < top - &dummy) {
		free(C->stack);// 如果是 NULL，free 没什么影响
		C->cap = top-&dummy;
		C->stack = malloc(C->cap);
	}
	C->size = top - &dummy;
     // 将以 dummy 为开始的 size 大小的数据保存到 C->stack 上
    // C->stack 是在 heap 上
	memcpy(C->stack, &dummy, C->size);
}

void
coroutine_yield(struct schedule * S) {
    // 获取当前正在执行的 coroutine id
	int id = S->running;
	assert(id >= 0);
	struct coroutine * C = S->co[id];
	assert((char *)&C > S->stack);
    // coroutine 自己会在 heap 上分配一个 stack，让 coroutine 把在 S->stack 上的内容
    // memcpy 到 C->stack 上
	_save_stack(C,S->stack + STACK_SIZE);
	C->status = COROUTINE_SUSPEND;
	S->running = -1;
    // 保存 coroutine 的 context，切到 main
	swapcontext(&C->ctx , &S->main);
}

int 
coroutine_status(struct schedule * S, int id) {
	assert(id>=0 && id < S->cap);
	if (S->co[id] == NULL) {
		return COROUTINE_DEAD;
	}
	return S->co[id]->status;
}

int 
coroutine_running(struct schedule * S) {
	return S->running;
}

