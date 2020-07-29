#include "lovely_fsm.h"
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------------
 * Managed internally, user needs lfsm_context_t (pointer) only
 * -------------------------------------------------------------------------- */
typedef struct lfsm_context_t {
    uint8_t is_active;
    uint8_t state_number_min;
    uint8_t state_number_max;
    uint8_t event_number_min;
    uint8_t event_number_max;
    uint8_t transition_count;
    uint8_t state_func_count;
    uint8_t event_count; // used multiple times, lets not calculate it all the time!
    uint8_t current_state;
    uint8_t previous_step_state;
    uint8_t event_queue_buffer[LFSM_EV_QUEUE_SIZE];
    lfsm_buf_callbacks_t buf_func;
    buffer_handle_type buffer_handle;
    void*   user_data;
    lfsm_transitions_t*      transition_table;
    lfsm_state_functions_t*  functions_table;
} lfsm_context_t;

typedef struct lfsm_system_t {
    lfsm_context_t contexts[LFSM_MAX_COUNT];
} lfsm_system_t;
lfsm_system_t lfsm_system;

typedef struct lfsm_lookup_element_t {
    lfsm_transitions_t* transition;
    lfsm_state_functions_t* functions;
} lfsm_lookup_element_t;

// public functions
lfsm_return_t fsm_add_event(lfsm_t context, uint8_t event);

// private functions
lfsm_t lfsm_get_unused_context();
lfsm_return_t lfsm_initialize_buffers(lfsm_t fsm);
lfsm_return_t lfsm_set_context_buf_callbacks(lfsm_t context, lfsm_buf_callbacks_t buffer_callbacks);
lfsm_return_t lfsm_set_context_buf_callbacks(lfsm_t new_fsm, lfsm_buf_callbacks_t buffer_callbacks);

void lfsm_find_state_event_min_max_count(lfsm_t context);
lfsm_transitions_t* lfsm_find_transition_to_execute(lfsm_context_t* fsm, uint8_t event);
lfsm_return_t lfsm_execute_transition(lfsm_context_t* fsm, lfsm_transitions_t* transition);
lfsm_state_functions_t* lfsm_get_state_function(lfsm_context_t* fsm, uint8_t state);
lfsm_return_t lfsm_run_callback(lfsm_context_t* fsm, lfsm_return_t (*function)());
lfsm_return_t lfsm_run_all_callbacks(lfsm_context_t* fsm);
uint8_t lfsm_no_event_queued(lfsm_context_t* fsm);
uint8_t lfsm_get_next_event(lfsm_context_t* fsm);

/* ---------------------------------------------------------------------------
 * MAIN FUNCTIONS FOR LIBRARY USERS
 * 
 * The main functions are
 * - init
 * - add event
 * - run
 * - deinit
 * -------------------------------------------------------------------------*/

// use lfsm_init #defined in header file instead :)
lfsm_t lfsm_init_func(lfsm_transitions_t* transitions, \
                        int trans_count,\
                        lfsm_state_functions_t* states,\
                        int state_count,\
                        lfsm_buf_callbacks_t buffer_callbacks, \
                        void* user_data, \
                        uint8_t initial_state)
{
    lfsm_t new_fsm = lfsm_get_unused_context();
    if (new_fsm) {
        new_fsm->functions_table = states;
        new_fsm->state_func_count = state_count;
        new_fsm->transition_table = transitions;
        new_fsm->transition_count = trans_count;
        new_fsm->current_state = initial_state;

        lfsm_set_context_buf_callbacks(new_fsm, buffer_callbacks);
        if (lfsm_initialize_buffers(new_fsm) == LFSM_OK) {
            lfsm_find_state_event_min_max_count(new_fsm);
            new_fsm->user_data = user_data;
            lfsm_run_all_callbacks(new_fsm);
            return new_fsm;
        }
    }
    return NULL;
}

// Adds an event to the event buffer.
lfsm_return_t fsm_add_event(lfsm_t context, uint8_t event) {
    lfsm_context_t* fsm = (lfsm_context_t*) context;

    int out_of_bounds = (event < fsm->event_number_min) || (event > fsm->event_number_max);
    if (out_of_bounds) return LFSM_ERROR;

    uint8_t error = context->buf_func.add(context->buffer_handle, event);
    if (error) return LFSM_ERROR;

    return LFSM_OK;
}

// Retrieves an event from the event buffer and handles state changes and
// callback function execution.
lfsm_return_t lfsm_run(lfsm_t context) {
    lfsm_context_t* fsm = (lfsm_context_t*) context;
    lfsm_transitions_t* transition;

    if (lfsm_no_event_queued(fsm)) {
        return LFSM_NOP;
    }

    uint8_t next_event = lfsm_get_next_event(fsm);

    transition = lfsm_find_transition_to_execute(fsm, next_event);
    if (transition == NULL) {
        return LFSM_NOP;
    }
    if (transition != NULL) {
        lfsm_execute_transition(fsm, transition);
    }

    lfsm_run_all_callbacks(fsm);

    if (lfsm_no_event_queued(fsm)) {
        return LFSM_OK;
    } else {
        return LFSM_MORE_QUEUED;
    }
}

lfsm_return_t lfsm_deinit(lfsm_t context) {
    memset((unsigned char*)context, 0, sizeof(lfsm_context_t));
    return LFSM_OK;
}

/* ---------------------------------------------------------------------------
 * - FUNCTIONS EMBEDDED IN MAIN USER FUNCTIONS
 * -------------------------------------------------------------------------*/

lfsm_t lfsm_get_unused_context() {
    lfsm_context_t* context;
    for (int index = 0 ; index < LFSM_MAX_COUNT ; index++) {
        if (!(lfsm_system.contexts[index].is_active)) {
            context = &lfsm_system.contexts[index];
            memset((unsigned char*)context, 0, sizeof(lfsm_context_t));
            context->current_state = LFSM_INVALID;
            context->previous_step_state = LFSM_INVALID;
            lfsm_system.contexts[index].is_active = 1;
            return context;
        }
    }
    return NULL;
}


#if (USE_LOVELY_BUFFER)
lfsm_return_t lfsm_set_lovely_buf_callbacks(lfsm_buf_callbacks_t* callbacks) {
    callbacks->system_init = buf_init_system;
    callbacks->init = buf_claim_and_init_buffer;
    callbacks->is_empty = buf_is_empty;
    callbacks->is_full = buf_is_full;
    callbacks->add  = buf_add_element;
    callbacks->read = buf_read_element;
    return LFSM_OK;
}
#endif

lfsm_return_t lfsm_initialize_buffers(lfsm_t fsm) {
#if (USE_LOVELY_BUFFER)
    buf_data_info_t data_info;
    data_info.array = fsm->event_queue_buffer;
    data_info.element_count = LFSM_EV_QUEUE_SIZE;
    data_info.element_size = sizeof(int);
    if (fsm->buf_func.init != NULL) {
        fsm->buffer_handle = fsm->buf_func.init(&data_info);
    } else {
        fsm->buffer_handle = NULL;
    }
#else
    fsm->buffer_handle = fsm->buf_func.init(fsm->event_queue_buffer, LFSM_EV_QUEUE_SIZE, sizeof(int));
#endif
    if (fsm->buffer_handle == NULL) return LFSM_ERROR;
    return LFSM_OK;
}


lfsm_return_t lfsm_set_context_buf_callbacks(lfsm_t context, lfsm_buf_callbacks_t buffer_callbacks){
    lfsm_context_t* fsm = (lfsm_context_t*)context;
    fsm->buf_func = buffer_callbacks;
    // todo: add checks for NULL?!
    return LFSM_OK;
}

// ----------------------------------------------------------------------------


void* lfsm_user_data(lfsm_t context) {
    return context->user_data;
}

lfsm_transitions_t* lfsm_get_transition_table(lfsm_t context) {
    lfsm_context_t* details = context;
    return details->transition_table;
}
int lfsm_get_transition_count(lfsm_t context) {
    lfsm_context_t* details = context;
    return details->transition_count;
}
lfsm_state_functions_t* lfsm_get_state_function_table(lfsm_t context) {
    lfsm_context_t* details = context;
    return details->functions_table;
}
int lfsm_get_state_function_count(lfsm_t context) {
    lfsm_context_t* details = context;
    return details->state_func_count;
}
int lfsm_get_state_min(lfsm_t context) {
    lfsm_context_t* details = context;
    return details->state_number_min;
}
int lfsm_get_state_max(lfsm_t context) {
    lfsm_context_t* details = context;
    return details->state_number_max;
}
int lfsm_get_event_min(lfsm_t context) {
    lfsm_context_t* details = context;
    return details->event_number_min;
}
int lfsm_get_event_max(lfsm_t context) {
    lfsm_context_t* details = context;
    return details->event_number_max;
}
uint8_t lfsm_get_state(lfsm_t context) {
    lfsm_context_t* details = context;
    return details->current_state;
}
uint8_t lfsm_set_state(lfsm_t context, uint8_t state) {
    lfsm_context_t* details = context;
    details->current_state = state;
    details->previous_step_state = state;
    return 0;
}
uint8_t lfsm_get_state_func_count(lfsm_t context) {
    lfsm_context_t* details = context;
    return details->state_func_count;
}
uint8_t lfsm_read_event_queue_element(lfsm_t context, uint8_t index) {
    lfsm_context_t* details = context;
    int out_of_bounds = (index < 0) || (index >= LFSM_EV_QUEUE_SIZE);

    if (out_of_bounds) {
        return LFSM_INVALID;
    }
    return details->event_queue_buffer[index];
}
uint8_t lfsm_read_event(lfsm_t context) {
    lfsm_context_t* details = context;
    uint8_t next_event = details->buf_func.read(details->buffer_handle);
    return next_event;
}

// --------------------------------------------------------------------------------
// runs through the transition table to find a transition that matches
// state/event with a 'true' return value on condition.
lfsm_transitions_t* lfsm_find_transition_to_execute(lfsm_context_t* fsm, uint8_t event) {
    int state_matches, event_matches;

    lfsm_transitions_t* transition = fsm->transition_table;
    for (int i = 0 ; i < fsm->transition_count ; i++, transition++) {
        state_matches = transition->current_state == fsm->current_state;
        event_matches = transition->event == event;

        if (state_matches && event_matches) {
            if (transition->condition == NULL) {
                return transition;
            }
            if (transition->condition(fsm)) {
                return transition;
            }
        }
    }
    return NULL;
}

lfsm_return_t lfsm_execute_transition(lfsm_context_t* fsm, lfsm_transitions_t* transition) {
    fsm->previous_step_state = fsm->current_state;
    fsm->current_state = transition->next_state;
    return LFSM_OK;
}


lfsm_state_functions_t* lfsm_get_state_function(lfsm_context_t* fsm, uint8_t state) {
    lfsm_state_functions_t* state_functions;
    int state_count, state_matches;

    state_count = fsm->state_number_max - fsm->state_number_min + 1;
    state_functions = fsm->functions_table;

    for (int i = 0; i < state_count ; i++, state_functions++) {
        state_matches = state_functions->state == state;
        if (state_matches) {
            return state_functions;
        }
    }
    return NULL;
}

lfsm_return_t lfsm_run_callback(lfsm_context_t* fsm, lfsm_return_t (*function)()) {
    if (function != NULL) {
        return function(fsm);
    }
    return LFSM_NOP;
}

lfsm_return_t lfsm_run_all_callbacks(lfsm_context_t* fsm) {
    lfsm_state_functions_t* callbacks_current;
    lfsm_state_functions_t* callbacks_previous;
    int state_changed;

    state_changed = fsm->previous_step_state != fsm->current_state;
    callbacks_current = lfsm_get_state_function(fsm, fsm->current_state);

    if (state_changed) {
        callbacks_previous = lfsm_get_state_function(fsm, fsm->previous_step_state);
        if ((callbacks_previous != NULL)  && (fsm->previous_step_state != LFSM_INVALID)) {
            lfsm_run_callback(fsm, callbacks_previous->on_exit);
        }
        if (callbacks_current != NULL) {
            lfsm_run_callback(fsm, callbacks_current->on_entry);
            lfsm_run_callback(fsm, callbacks_current->on_run);
        }
    } else {
        if (callbacks_current != NULL) {
            lfsm_run_callback(fsm, callbacks_current->on_run);
        }
    }
    return LFSM_OK;
}

uint8_t lfsm_no_event_queued(lfsm_context_t* fsm) {
    int nothing_to_do = fsm->buf_func.is_empty(fsm->buffer_handle);
    return nothing_to_do;
}

uint8_t lfsm_get_next_event(lfsm_context_t* fsm) {
    uint8_t next_event;
    int out_of_bounds;

    next_event = fsm->buf_func.read(fsm->buffer_handle);
    out_of_bounds = (next_event > fsm->event_number_max) || (next_event < fsm->event_number_min);
    if (out_of_bounds) {
        return LFSM_INVALID;
    }
    return next_event;
}

uint8_t max(uint8_t a, uint8_t b) {
    if (a > b) return a;
    return b;
}

uint8_t min(uint8_t a, uint8_t b) {
    if (a < b) return a;
    return b;
}

void lfsm_find_state_event_min_max_count(lfsm_t context) {
    int list_length = context->transition_count;
    lfsm_transitions_t* transition = context->transition_table;
    uint8_t max_state = 0;
    uint8_t min_state  = 255;
    uint8_t max_event = 0;
    uint8_t min_event  = 255;

    for (int i = 0; i < list_length ; i++, transition++) {
        min_event = min(min_event, transition->event);
        max_event = max(max_event, transition->event);

        min_state = min(min_state, min(transition->current_state, transition->next_state));
        max_state = max(max_state, max(transition->current_state, transition->next_state));
    }
    context->state_number_min = min_state;
    context->state_number_max = max_state;
    context->event_number_min = min_event;
    context->event_number_max = max_event;
    context->event_count = max_event - min_event + 1;
}

int lfsm_always() {
    return 1;
}

