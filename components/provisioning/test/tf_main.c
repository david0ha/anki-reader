// Test registry + runner for the tiny host framework (tf.h).
#include "tf.h"

int tf_failures = 0;
int tf_checks = 0;

#define TF_MAX 512
static struct {
    const char *name;
    tf_func fn;
} g_cases[TF_MAX];
static int g_count = 0;

void tf_register(const char *name, tf_func fn)
{
    if (g_count < TF_MAX) {
        g_cases[g_count].name = name;
        g_cases[g_count].fn = fn;
        g_count++;
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);  // unbuffered: output survives an abort mid-test
    for (int i = 0; i < g_count; i++) {
        int before = tf_failures;
        g_cases[i].fn();
        const char *status = (tf_failures == before) ? "ok  " : "FAIL";
        printf("%s %s\n", status, g_cases[i].name);
    }
    printf("\n%d tests, %d checks, %d failures\n", g_count, tf_checks, tf_failures);
    return tf_failures ? 1 : 0;
}
