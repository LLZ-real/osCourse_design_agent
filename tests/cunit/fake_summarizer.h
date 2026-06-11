#ifndef CUNIT_FAKE_SUMMARIZER_H
#define CUNIT_FAKE_SUMMARIZER_H

void fake_summarizer_set_response(const char *content);
void fake_summarizer_force_error(int yes);
int fake_summarizer_call_count(void);
const char *fake_summarizer_last_model(void);
void fake_summarizer_reset(void);

#endif
