/*
 * walls.c
 */

/* 
 * TODO:
 *  
 *  - Prohibit wall placing over lines
 *  - Draw area and elements outside border
 *  - Implement error handling
 *  - Implement solved flash
 *  - Implement line dragging
 *  - Implement state save / recall
 *  - Solver:
 *      - Optimize line reducer / wall placement
 *      - Implement stride solver
 *      - Implement board partition check
 *      - Implement exit parity check
 *      - Implement area parity check
 *      - Implement backtracking
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <math.h>

#include "puzzles.h"

/* Macro ickery copied from slant.c */
#define DIFFLIST(A) \
    A(EASY,Easy,e) \
    A(TRICKY,Tricky,t) \
    A(HARD,Hard,h)
#define ENUM(upper,title,lower) DIFF_ ## upper,
#define TITLE(upper,title,lower) #title,
#define ENCODE(upper,title,lower) #lower
#define CONFIG(upper,title,lower) ":" #title
enum { DIFFLIST(ENUM) DIFFCOUNT };
static char const *const walls_diffnames[] = { DIFFLIST(TITLE) "(count)" };
static char const walls_diffchars[] = DIFFLIST(ENCODE);
#define DIFFCONFIG DIFFLIST(CONFIG)

#define BLANK (0x00)
#define R (0x01)
#define U (0x02)
#define L (0x04)
#define D (0x08)

enum {
    COL_BACKGROUND,
    COL_FLOOR_A,
    COL_FLOOR_B,
    COL_FIXED,
    COL_WALL,
    COL_GRID,
    COL_LINE,
    COL_DRAGLINE,
    COL_ERROR,
    NCOLOURS
};

struct game_params {
    int w, h;
    int difficulty;
};

struct shared_state {
    int w, h, diff, wh, nw;
    char *fixed; /* size (w+1)*h + w*(h+1): fixed walls */
    int refcnt;
};

struct game_state {
    struct shared_state *shared;
    char *lines;        /* size w*h: lines placed */
    char *errors;       /* size w*h: errors detected */
    char *walls;        /* size (w+1)*h + w*(h+1): placed walls */
    int completed, used_solve;
};

#define DEFAULT_PRESET 0
static const struct game_params walls_presets[] = {
    {5, 4,      DIFF_EASY},
    {4, 5,      DIFF_EASY},
};


static game_params *default_params(void)
{
    game_params *ret = snew(game_params);
    *ret = walls_presets[DEFAULT_PRESET];
    return ret;
}

static int game_fetch_preset(int i, char **name, game_params **params)
{
    game_params *ret;
    char buf[64];

    if (i < 0 || i >= lenof(walls_presets)) return FALSE;

    ret = default_params();
    *ret = walls_presets[i]; /* struct copy */
    *params = ret;

    sprintf(buf, "%dx%d %s",
            ret->w, ret->h,
            walls_diffnames[ret->difficulty]);
    *name = dupstr(buf);

    return TRUE;
}

static void free_params(game_params *params)
{
    sfree(params);
}

static game_params *dup_params(const game_params *params)
{
    game_params *ret = snew(game_params);
    *ret = *params;		       /* structure copy */
    return ret;
}

static void decode_params(game_params *params, char const *string)
{
    params->w = params->h = atoi(string);
    while (*string && isdigit((unsigned char) *string)) ++string;
    if (*string == 'x') {
        string++;
        params->h = atoi(string);
        while (*string && isdigit((unsigned char)*string)) string++;
    }

    params->difficulty = DIFF_EASY;

    if (*string == 'd') {
        int i;
        string++;
        for (i = 0; i < DIFFCOUNT; i++)
            if (*string == walls_diffchars[i])
                params->difficulty = i;
        if (*string) string++;
    }
    return;
}

static char *encode_params(const game_params *params, int full)
{
    char buf[256];
    sprintf(buf, "%dx%d", params->w, params->h);
    if (full)
        sprintf(buf + strlen(buf), "d%c",
                walls_diffchars[params->difficulty]);
    return dupstr(buf);
}

static config_item *game_configure(const game_params *params)
{
    config_item *ret;
    char buf[64];

    ret = snewn(4, config_item);

    ret[0].name = "Width";
    ret[0].type = C_STRING;
    sprintf(buf, "%d", params->w);
    ret[0].sval = dupstr(buf);
    ret[0].ival = 0;

    ret[1].name = "Height";
    ret[1].type = C_STRING;
    sprintf(buf, "%d", params->h);
    ret[1].sval = dupstr(buf);
    ret[1].ival = 0;

    ret[2].name = "Difficulty";
    ret[2].type = C_CHOICES;
    ret[2].sval = DIFFCONFIG;
    ret[2].ival = params->difficulty;

    ret[3].name = NULL;
    ret[3].type = C_END;
    ret[3].sval = NULL;
    ret[3].ival = 0;

    return ret;
}

static game_params *custom_params(const config_item *cfg)
{
    game_params *ret = snew(game_params);

    ret->w = atoi(cfg[0].sval);
    ret->h = atoi(cfg[1].sval);
    ret->difficulty = cfg[2].ival;

    return ret;
}

static char *validate_params(const game_params *params, int full)
{
    if (params->w < 2) return "Width must be at least three";
    if (params->h < 2) return "Height must be at least three";
    if (params->difficulty < 0 || params->difficulty >= DIFFCOUNT)
        return "Unknown difficulty level";
    return NULL;
}

/* ----------------------------------------------------------------------
 * Solver.
 */

#define SOLVE_SOLVEABLE 1
#define SOLVE_UNSOLVEABLE 2
#define SOLVE_AMBIGUOUS 3

int grid_to_wall(int g, int w, int h, int dir) {
    int wall = -1;
    int x = g % w;
    int y = g / w;
    switch(dir) {
        case L: wall = (w+1)*y + x; break;
        case R: wall = (w+1)*y + x + 1; break;
        case U: wall = (w+1)*h + w*y + x; break;
        case D: wall = (w+1)*h + w*y + x + w; break;
    }
    assert(wall >= 0);
    return wall;
}

int wall_to_grid(int wall, int w, int h, int dir) {
    int grid = -1;
    int ws = (w+1)*h;
    int x = (wall < ws) ? wall % (w+1) : (wall-ws) % w;
    int y = (wall < ws) ? wall / (w+1) : (wall-ws) / w;
    if (wall < ws) {
        switch(dir) {
            case L: grid = (x > 0) ? y*w + (x-1) : -1; break;
            case R: grid = (x < w) ? y*w + x     : -1; break;
        }
    }
    else {
        switch(dir) {
            case U: grid = (y > 0) ? (y-1)*w + x : -1; break;
            case D: grid = (y < h) ? y*w + x : -1; break;
        }
    }
    return grid;
}

int check_solution(int w, int h, char *result) {
    int i;
    int *dsf;
    int first_eq;
    int exit1, exit2;
    
    dsf = snewn(w*h, int);
    dsf_init(dsf, w*h);
    
    exit1 = exit2 = -1;
    
    for (i=0;i<w*h;i++) {
        int r = result[i];
        int x = i % w;
        int y = i / w;
        if (r == 0x00 || r == L || r == R || r == U || r == D) {
            sfree(dsf);
            return SOLVE_UNSOLVEABLE;
        }
        if (r != (L|R) && r != (L|U) && r != (L|D) &&
            r != (R|U) && r != (R|D) && r != (U|D) ) {
            sfree(dsf);
            return SOLVE_AMBIGUOUS;
        }
        if ((r & L) && (x > 0))   dsf_merge(dsf, i, i-1);
        if ((r & R) && (x < w-1)) dsf_merge(dsf, i, i+1);
        if ((r & U) && (y > 0))   dsf_merge(dsf, i, i-w);
        if ((r & D) && (y < h-1)) dsf_merge(dsf, i, i+w);
        
        if ((r & L) && (x == 0)) {
            if (exit2 != -1) { sfree(dsf); return SOLVE_UNSOLVEABLE; }
            if (exit1 != -1) exit2 = i; else exit1 = i;
        }
        if ((r & R) && (x == w-1)) {
            if (exit2 != -1) { sfree(dsf); return SOLVE_UNSOLVEABLE; }
            if (exit1 != -1) exit2 = i; else exit1 = i;
        }
        if ((r & U) && (y == 0)) {
            if (exit2 != -1) { sfree(dsf); return SOLVE_UNSOLVEABLE; }
            if (exit1 != -1) exit2 = i; else exit1 = i;
        }
        if ((r & D) && (y == h-1)) {
            if (exit2 != -1) { sfree(dsf); return SOLVE_UNSOLVEABLE; }
            if (exit1 != -1) exit2 = i; else exit1 = i;
        }
    }
    
    if (exit1 == -1 || exit2 == -1) { sfree(dsf); return SOLVE_UNSOLVEABLE; }
    
    first_eq = dsf_canonify(dsf, 0);
    for (i=1;i<w*h;i++) {
        if (dsf_canonify(dsf, i) != first_eq) {
            sfree(dsf);
            return SOLVE_UNSOLVEABLE;
        }
    }
    
    sfree(dsf);    
    return SOLVE_SOLVEABLE;
}

#define TC_CON 1
#define TC_DIS 2
#define TC_UNK 3

int walls_solve(int w, int h, char *clues, char *result, int diff) {
    
    int i;
    int ws = (w + 1) * h + w * (h + 1);
    char *tc = snewn(ws, char);
        
    for (i=0;i<w*h;i++) result[i] = L|R|U|D;
    for (i=0;i<ws;i++) tc[i] = clues[i] ? TC_DIS : TC_UNK;

    while(TRUE) {
        unsigned char done_something = FALSE;
        
        for (i=0;i<w*h;i++) {
            int lw = grid_to_wall(i, w, h, L);
            int rw = grid_to_wall(i, w, h, R);
            int uw = grid_to_wall(i, w, h, U);
            int dw = grid_to_wall(i, w, h, D);
/*            int x = i % w;
            int y = i / w;
*/            
            if (tc[lw] == TC_DIS && (result[i] & L)) { 
                done_something = TRUE;
                result[i] &= (R|U|D);
            }
            if (tc[rw] == TC_DIS && (result[i] & R)) { 
                done_something = TRUE;
                result[i] &= (L|U|D);
            }
            if (tc[uw] == TC_DIS && (result[i] & U)) { 
                done_something = TRUE;
                result[i] &= (L|R|D);
            }
            if (tc[dw] == TC_DIS && (result[i] & D)) { 
                done_something = TRUE;
                result[i] &= (L|R|U);
            }
            
            if (tc[lw] == TC_UNK && !(result[i] & L)) {
                done_something = TRUE;
                tc[lw] = TC_DIS;
            }
            if (tc[rw] == TC_UNK && !(result[i] & R)) {
                done_something = TRUE;
                tc[rw] = TC_DIS;
            }
            if (tc[uw] == TC_UNK && !(result[i] & U)) {
                done_something = TRUE;
                tc[uw] = TC_DIS;
            }
            if (tc[dw] == TC_UNK && !(result[i] & D)) {
                done_something = TRUE;
                tc[dw] = TC_DIS;
            }
                     
            if (result[i] == (L|R) && 
                (tc[lw] != TC_CON || tc[rw] != TC_CON || tc[uw] != TC_DIS || tc[dw] != TC_DIS)) {
                done_something = TRUE;
                tc[lw] = tc[rw] = TC_CON;
                tc[uw] = tc[dw] = TC_DIS;
            }
            if (result[i] == (L|U) && 
                (tc[lw] != TC_CON || tc[rw] != TC_DIS || tc[uw] != TC_CON || tc[dw] != TC_DIS)) {
                done_something = TRUE;
                tc[lw] = tc[uw] = TC_CON;
                tc[rw] = tc[dw] = TC_DIS;
            }
            if (result[i] == (L|D) && 
                (tc[lw] != TC_CON || tc[rw] != TC_DIS || tc[uw] != TC_DIS || tc[dw] != TC_CON)) {
                done_something = TRUE;
                tc[lw] = tc[dw] = TC_CON;
                tc[rw] = tc[uw] = TC_DIS;
            }
            if (result[i] == (R|U) && 
                (tc[lw] != TC_DIS || tc[rw] != TC_CON || tc[uw] != TC_CON || tc[dw] != TC_DIS)) {
                done_something = TRUE;
                tc[rw] = tc[uw] = TC_CON;
                tc[lw] = tc[dw] = TC_DIS;
            }
            if (result[i] == (R|D) && 
                (tc[lw] != TC_DIS || tc[rw] != TC_CON || tc[uw] != TC_DIS || tc[dw] != TC_CON)) {
                done_something = TRUE;
                tc[rw] = tc[dw] = TC_CON;
                tc[lw] = tc[uw] = TC_DIS;
            }
            if (result[i] == (U|D) && 
                (tc[lw] != TC_DIS || tc[rw] != TC_DIS || tc[uw] != TC_CON || tc[dw] != TC_CON)) {
                done_something = TRUE;
                tc[uw] = tc[dw] = TC_CON;
                tc[lw] = tc[rw] = TC_DIS;
            }
            
            if (tc[lw] == TC_CON && tc[rw] == TC_CON && !(result[i] == (L|R))) {
                done_something = TRUE;
                result[i] = (L|R);
            }
            if (tc[lw] == TC_CON && tc[uw] == TC_CON && !(result[i] == (L|U))) {
                done_something = TRUE;
                result[i] = (L|U);
            }
            if (tc[lw] == TC_CON && tc[dw] == TC_CON && !(result[i] == (L|D))) {
                done_something = TRUE;
                result[i] = (L|D);
            }
            if (tc[rw] == TC_CON && tc[uw] == TC_CON && !(result[i] == (R|U))) {
                done_something = TRUE;
                result[i] = (R|U);
            }
            if (tc[rw] == TC_CON && tc[dw] == TC_CON && !(result[i] == (R|D))) {
                done_something = TRUE;
                result[i] = (R|D);
            }
            if (tc[uw] == TC_CON && tc[dw] == TC_CON && !(result[i] == (U|D))) {
                done_something = TRUE;
                result[i] = (U|D);
            }
                     
        }
                     
        if (!done_something) break;
    }
    
    sfree(tc);    
    return check_solution(w, h, result);
}

static game_state *new_state(const game_params *params) {
    int i;
    game_state *state = snew(game_state);

    state->completed = state->used_solve = FALSE;

    state->shared = snew(struct shared_state);
    state->shared->w = params->w;
    state->shared->h = params->h;
    state->shared->diff = params->difficulty;
    state->shared->wh = params->w * params->h;
    state->shared->nw = (params->w + 1)*params->h + params->w*(params->h + 1);
    state->shared->fixed = snewn(state->shared->nw, char);
    state->shared->refcnt = 1;

    state->lines  = snewn(state->shared->wh, char);
    state->errors = snewn(state->shared->wh, char);
    state->walls  = snewn(state->shared->nw, char);

    for (i = 0; i < state->shared->wh; i++)
        state->lines[i] = state->errors[i]= BLANK;
    for (i=0;i<state->shared->nw;i++)
        state->shared->fixed[i] = state->walls[i] = FALSE;


    return state;
}

static game_state *new_game(midend *me, const game_params *params,
                            const char *desc) {
    int i, c;
    game_state *state = new_state(params);

    i = 0;
    while (*desc) {
        if(isdigit((unsigned char)*desc)) {            
            for (c = atoi(desc); c > 0; c--) {
                state->walls[i] = TRUE;
                state->shared->fixed[i] = TRUE;
                i++;
            }
            while (*desc && isdigit((unsigned char)*desc)) desc++;
        }
        else if(*desc >= 'a' && *desc <= 'z') {
            for (c = *desc - 'a' + 1; c > 0; c--) {
                state->walls[i] = FALSE;
                state->shared->fixed[i] = FALSE;
                i++;
            }
            if (*desc < 'z' && i < state->shared->nw) {
                state->walls[i] = TRUE;
                state->shared->fixed[i] = TRUE;
                i++;
            }
            desc++;
        }
    }  

    assert(i == state->shared->nw);

    return state;
}

static game_state *dup_game(const game_state *state) {
    int i;
    game_state *ret = snew(game_state);

    ret->shared = state->shared;
    ret->completed = state->completed;
    ret->used_solve = state->used_solve;
    ++ret->shared->refcnt;

    ret->lines = snewn(state->shared->wh, char);
    ret->errors = snewn(state->shared->wh, char);
    ret->walls = snewn(state->shared->nw, char);
    for (i = 0; i < state->shared->wh; i++) {
        ret->lines[i] = state->lines[i];
        ret->errors[i] = state->errors[i];
    }
    for (i=0;i<state->shared->nw; i++) {
        ret->walls[i] = state->walls[i];
    }
  
    return ret;
}

static void free_game(game_state *state) {
    assert(state);
    if (--state->shared->refcnt == 0) {
        sfree(state->shared->fixed);
        sfree(state->shared);
    }
    sfree(state->lines);
    sfree(state->errors);
    sfree(state->walls);
    sfree(state);
}

static char *game_text_format(const game_state *state);

void reverse_path(int i1, int i2, int *pathx, int *pathy) {
    int i;
    int ilim = (i2-i1+1)/2;
    int temp;
    for (i=0; i<ilim; i++)
    {
        temp = pathx[i1+i];
        pathx[i1+i] = pathx[i2-i];
        pathx[i2-i] = temp;

        temp = pathy[i1+i];
        pathy[i1+i] = pathy[i2-i];
        pathy[i2-i] = temp;
    }
}

int backbite_left(int step, int n, int *pathx, int *pathy, int w, int h) {
    int neighx, neighy;
    int i, inPath = FALSE;
    switch(step) {
        case L: neighx = pathx[0]-1; neighy = pathy[0];   break;
        case R: neighx = pathx[0]+1; neighy = pathy[0];   break;
        case U: neighx = pathx[0];   neighy = pathy[0]-1; break;
        case D: neighx = pathx[0];   neighy = pathy[0]+1; break; 
        default: neighx = -1; neighy = -1; break;       
    }
    if (neighx < 0 || neighx >= w || neighy < 0 || neighy >= h) return n;
    
    for (i=1;i<n;i+=2) {
        if (neighx == pathx[i] && neighy == pathy[i]) { inPath = TRUE; break; }
    }
    if (inPath) {
        reverse_path(0, i-1, pathx, pathy);
    }
    else {
        reverse_path(0, n-1, pathx, pathy);        
        pathx[n] = neighx;
        pathy[n] = neighy;
        n++;        
    }
    
    return n;
}

int backbite_right(int step, int n, int *pathx, int *pathy, int w, int h) {
    int neighx, neighy;
    int i, inPath = FALSE;
    switch(step) {
        case L: neighx = pathx[n-1]-1; neighy = pathy[n-1];   break;
        case R: neighx = pathx[n-1]+1; neighy = pathy[n-1];   break;
        case U: neighx = pathx[n-1];   neighy = pathy[n-1]-1; break;
        case D: neighx = pathx[n-1];   neighy = pathy[n-1]+1; break; 
        default: neighx = -1; neighy = -1; break;              
    }
    if (neighx < 0 || neighx >= w || neighy < 0 || neighy >= h) return n;
    for (i=n-2;i>=0;i-=2) {
        if (neighx == pathx[i] && neighy == pathy[i]) { inPath = TRUE; break; }
    }
    if (inPath) {
        reverse_path(i+1, n-1, pathx, pathy);
    }
    else {
        pathx[n] = neighx;
        pathy[n] = neighy;
        n++;        
    }
    
    return n;
}

int random_step(random_state *rs) {
    switch(random_upto(rs, 4)) {
        case 0: return L; break;
        case 1: return R; break;
        case 2: return U; break;
        case 3: return D; break;
        default: return 0;       
    }    
}

int backbite(int n, int *pathx, int *pathy, int w, int h, random_state *rs) {
    if (random_upto(rs, 2) == 0) return backbite_left(random_step(rs), n, pathx, pathy, w, h);
    else                         return backbite_right(random_step(rs), n, pathx, pathy, w, h);
}

void generate_hamiltonian_path(game_state *state, random_state *rs) {
    int w = state->shared->w;
	int h = state->shared->h;
    int *pathx = snewn(w*h, int);
    int *pathy = snewn(w*h, int);
    int n = 1;
    
    pathx[0] = random_upto(rs, w);
    pathy[0] = random_upto(rs, h);
    
    while (n < w*h) {
        n = backbite(n, pathx, pathy, w, h, rs);
    }
  
    while (!(pathx[0] == 0 || pathx[0] == w-1) && !(pathy[0] == 0 || pathy[0] == h-1)) {
        backbite_left(random_step(rs), n, pathx, pathy, w, h);
    }
    
    while (!(pathx[n-1] == 0 || pathx[n-1] == w-1) && !(pathy[n-1] == 0 || pathy[n-1] == h-1)) {
        backbite_right(random_step(rs), n, pathx, pathy, w, h);
    }

    for (n=0;n<w*h;n++) {
        int pos = pathx[n] + pathy[n]*w;
        
        if (n < (w*h-1)) {
            if      (pathx[n+1] - pathx[n] ==  1) state->walls[grid_to_wall(pos, w, h, R)] = FALSE;
            else if (pathx[n+1] - pathx[n] == -1) state->walls[grid_to_wall(pos, w, h, L)] = FALSE;
            else if (pathy[n+1] - pathy[n] ==  1) state->walls[grid_to_wall(pos, w, h, D)] = FALSE;
            else if (pathy[n+1] - pathy[n] == -1) state->walls[grid_to_wall(pos, w, h, U)] = FALSE;
        }
        if (n == 0 || n == (w*h)-1) {
            if      (pathx[n] == 0)   state->walls[grid_to_wall(pos, w, h, L)] = FALSE;
            else if (pathx[n] == w-1) state->walls[grid_to_wall(pos, w, h, R)] = FALSE;
            else if (pathy[n] == 0)   state->walls[grid_to_wall(pos, w, h, U)] = FALSE;
            else if (pathy[n] == h-1) state->walls[grid_to_wall(pos, w, h, D)] = FALSE;
        }
    }
    
    sfree(pathx);
    sfree(pathy);

    return;
}
static char *validate_desc(const game_params *params, const char *desc);

static char *new_game_desc(const game_params *params, random_state *rs,
                           char **aux, int interactive) {
    int i,j;
    game_state *new;
    char *desc, *e;
	int erun, wrun;
    int *wallindices;
    int wallnum;

    int ws = (params->w + 1) * params->h + params->w * (params->h + 1);
    int w = params->w;
	int h = params->h;
    
    new = new_state(params);

    for (i=0;i<ws;i++)
        new->walls[i] = TRUE;

    generate_hamiltonian_path(new, rs);
  
    /*
     * Remove as many walls as possible while retaining solubility.
     */
    wallindices = snewn(ws, int);
    wallnum = 0;
    for (i=0;i<ws;i++) {
        if (new->walls[i]) {
            wallindices[wallnum++] = i;
        }        
    }
    shuffle(wallindices, wallnum, sizeof(int), rs);
 
    for (i=0;i<wallnum;i++) {
        int index = wallindices[i];
        new->walls[index] = FALSE;
        for (j=0;j<w*h;j++) new->lines[j] = 0x00;
        if (walls_solve(w, h, new->walls, new->lines, 0) != SOLVE_SOLVEABLE) {
            new->walls[index] = TRUE;
        }
    }
    sfree(wallindices);
    for (i=0;i<w*h;i++) new->lines[i] = 0x00;
 
    sfree(game_text_format(new)); printf("\n");

    
    /* We have a valid puzzle! */
    
    /* Encode walls */
    desc = snewn(ws + (w*h),char);
    e = desc;
	erun = wrun = 0;
	for(i = 0; i < ws; i++) {
		if(!new->walls[i] && wrun > 0) {
			e += sprintf(e, "%d", wrun);
			wrun = erun = 0;
		}
		else if(new->walls[i] && erun > 0) {
            while (erun >= 26) {
                *e++ = 'z';
                erun -= 26;
            }
            if (erun == 0) {
                wrun = 0;
            }
            else {
                *e++ = ('a' + erun - 1);
                erun = 0; wrun = -1;
            }
		}
		if(!new->walls[i]) erun++;
		else   	           wrun++;
	}
	if(wrun > 0) e += sprintf(e, "%d", wrun);
	if(erun > 0) *e++ = ('a' + erun - 1);
    *e++ = '\0';
	
    free_game(new);
    printf("New desc: %s\n", desc);
    assert (validate_desc(params, desc) == NULL);
    
    return desc;
}

static char *validate_desc(const game_params *params, const char *desc) {
    int wsl = 0;
    int ws = (params->w + 1) * params->h + params->w * (params->h + 1);
    
    while (*desc) {
        if(isdigit((unsigned char)*desc)) {
            wsl += atoi(desc);
            while (*desc && isdigit((unsigned char)*desc)) desc++;
        }
        else if(*desc >= 'a' && *desc <= 'z') {
            wsl += *desc - 'a' + 1 + (*desc != 'z' ? 1 : 0);
            desc++;
            if (!(*desc) && wsl == ws + 1) wsl--;
        }
        else
            return "Faulty game description";
    }
    if (wsl < ws) return "Too few walls in game description";
    if (wsl > ws) return "Too many walls in game description";

    return NULL;
}

static char *solve_game(const game_state *state, const game_state *currstate,
                        const char *aux, char **error) {

    int w = state->shared->w, h = state->shared->h, i;
    char *move = snewn(w*h*40, char), *p = move;

    game_state *solve_state = dup_game(state);
    walls_solve(w,h,solve_state->walls, solve_state->lines,0);

    *p++ = 'S';
    for (i = 0; i < w*h; i++) {
        p += sprintf(p, ";P%d,%d", i, solve_state->lines[i]);
    }
    *p++ = '\0';
    move = sresize(move, p - move, char);
    free_game(solve_state);

    return move;
}

static int game_can_format_as_text_now(const game_params *params) {
    return TRUE;
}

static char *game_text_format(const game_state *state) {
    int x,y;
    char *ret, *p;
    char iswall, isline, isleft, isright;
    int w = state->shared->w, h = state->shared->h;
    ret = snewn((9*w*h) + (3*w) + (6*h) + 1, char);

    p = ret;

    for (y=0;y<h;y++) {
        for (x=0;x<w;x++) {
            iswall = state->walls[(w + 1)*h + y*w + x];
            isline = (state->lines[y*w + x] & U) > 0x00;
            *p++ = '+';
            *p++ = iswall ? '-' : ' ';
            *p++ = isline ? '*' : iswall ? '-' : ' ';
            *p++ = iswall ? '-' : ' ';
        }
        *p++ = '+'; *p++ = '\n';
        for (x=0;x<w;x++) {
            iswall = state->walls[y*(w+1) + x];
            isleft = (state->lines[y*w + x] & L) > 0x00;
            isright = (state->lines[y*w + x] & R) > 0x00;
            *p++ = isleft ? '*' : iswall ? '|' : ' ';
            *p++ = isleft ? '*' : ' ';
            *p++ = state->lines[y*w + x] != BLANK ? '*' : ' ';
            *p++ = isright ? '*' : ' ';
        }
        iswall = state->walls[y*(w+1) + w];
        isright = (state->lines[y*w + w-1] & R) > 0x00;
        *p++ = isright ? '*' : iswall ? '|' : ' '; 
        *p++ = '\n';        
    }
    for (x=0;x<w;x++) {
        iswall = state->walls[((w+1) * h) + 
                              (w * h) + x];
        isline = (state->lines[(h-1)*w + x] & D) > 0x00;
        *p++ = '+';
        *p++ = iswall ? '-' : ' ';
        *p++ = isline ? '*' : iswall ? '-' : ' ';
        *p++ = iswall ? '-' : ' ';
    }
    *p++ = '+'; *p++ = '\n'; *p = 0x00;
    
    printf("%s", ret);
    return ret;
}

static game_ui *new_ui(const game_state *state) {
    return NULL;
}

static void free_ui(game_ui *ui) {
}

static char *encode_ui(const game_ui *ui) {
    return NULL;
}

static void decode_ui(game_ui *ui, const char *encoding) {
}

static void game_changed_state(game_ui *ui, const game_state *oldstate,
                               const game_state *newstate) {
}

#define PREFERRED_TILE_SIZE 48
#define TILESIZE (ds->tilesize)
#define BORDER (3*TILESIZE/4)
#define COORD(x) ( (x) * TILESIZE + BORDER )
#define CENTERED_COORD(x) ( COORD(x) + TILESIZE/2 )
#define FROMCOORD(x) ( ((x) - BORDER) / TILESIZE )

struct game_drawstate {
    int tilesize;
    int started;
};

static char *interpret_move(const game_state *state, game_ui *ui,
                            const game_drawstate *ds,
                            int x, int y, int button) {
    char buf[80];
    int type = 0;
    int w = state->shared->w;
    int h = state->shared->h;
    int fx = FROMCOORD(x);
    int fy = FROMCOORD(y);
    int lx = x - (fx * TILESIZE) - BORDER;
    int ly = y - (fy * TILESIZE) - BORDER;
    
    if      (lx < (TILESIZE/2 - abs(TILESIZE/2 - ly))) type = L;
    else if (lx > (TILESIZE/2 + abs(TILESIZE/2 - ly))) type = R;
    else if (ly < (TILESIZE/2 - abs(TILESIZE/2 - lx))) type = U;
    else if (ly > (TILESIZE/2 + abs(TILESIZE/2 - lx))) type = D;
        
    if (type == 0) return NULL;    
    
    if (fx ==  w && type == L) { fx = w-1; type = R; }
    if (fx == -1 && type == R) { fx =   0; type = L; }
    if (fy ==  h && type == U) { fy = h-1; type = D; }
    if (fy == -1 && type == D) { fy =   0; type = U; }
    
    if (button == LEFT_BUTTON) {
        int pos = fx+fy*w;
        int pos2 = -1;
        int type2 = -1;
        if (state->walls[grid_to_wall(pos,w,h,type)]) return NULL;
        if (type == L && fx > 0)     { pos2 = (fx-1)+fy*w; type2 = R; }
        if (type == R && fx < (w-1)) { pos2 = (fx+1)+fy*w; type2 = L; }
        if (type == U && fy > 0)     { pos2 = fx+(fy-1)*w; type2 = D; }
        if (type == D && fy < (h-1)) { pos2 = fx+(fy+1)*w; type2 = U; }
        
        if (type2 > 0) sprintf(buf,"F%d,%d;F%d,%d",pos,type,pos2,type2);
        else           sprintf(buf,"F%d,%d",pos,type);
        return dupstr(buf);
    }
    else if (button == RIGHT_BUTTON) {
        int pos = grid_to_wall(fx+fy*w,w,h,type);
        if (state->shared->fixed[pos]) return NULL;
        sprintf(buf, "W%d",pos);
        return dupstr(buf);        
    }
    
    return NULL;
}

static game_state *execute_move(const game_state *state, const char *move) {
    char c;
    int g, t, n;
    game_state *ret = dup_game(state);

    printf("Move: %s\n", move);

    while (*move) {
        c = *move;
        if (c == 'S') {
            ret->used_solve = TRUE;
            move++;
        } else if (c == 'W') {
            move++;
            if (sscanf(move, "%d%n", &g, &n) < 1) goto badmove;
            ret->walls[g] = !ret->walls[g];
            move += n;
        }
        else if (c == 'F' || c == 'P') {
            move++;
            if (sscanf(move, "%d,%d%n", &g, &t, &n) < 2) goto badmove;
            if (c == 'F') ret->lines[g] ^= (char)t;
            if (c == 'P') ret->lines[g] = (char)t;
            move += n;
        }

        if (*move == ';')
            move++;
        else if (*move)
            goto badmove;
    }

    return ret;

badmove:
    printf("Bad move!\n");
    free_game(ret);
    return NULL;
}

/* ----------------------------------------------------------------------
 * Drawing routines.
 */
 
static void game_compute_size(const game_params *params, int tilesize,
                              int *x, int *y) {
   /* Ick: fake up `ds->tilesize' for macro expansion purposes */
    struct { int tilesize; } ads, *ds = &ads;
    ads.tilesize = tilesize;

    *x = (params->w) * TILESIZE + 2 * BORDER;
    *y = (params->h) * TILESIZE + 2 * BORDER;
}

static void game_set_size(drawing *dr, game_drawstate *ds,
                          const game_params *params, int tilesize) {
    ds->tilesize = tilesize;
}

static float *game_colours(frontend *fe, int *ncolours) {
    float *ret = snewn(3 * NCOLOURS, float);

    frontend_default_colour(fe, &ret[COL_BACKGROUND * 3]);

    ret[COL_FLOOR_A * 3 + 0] = 0.9F;
    ret[COL_FLOOR_A * 3 + 1] = 0.9F;
    ret[COL_FLOOR_A * 3 + 2] = 0.9F;

    ret[COL_FLOOR_B * 3 + 0] = 0.8F;
    ret[COL_FLOOR_B * 3 + 1] = 0.8F;
    ret[COL_FLOOR_B * 3 + 2] = 0.8F;

    ret[COL_FIXED * 3 + 0] = 0.1F;
    ret[COL_FIXED * 3 + 1] = 0.1F;
    ret[COL_FIXED * 3 + 2] = 0.1F;

    ret[COL_WALL * 3 + 0] = 0.5F;
    ret[COL_WALL * 3 + 1] = 0.5F;
    ret[COL_WALL * 3 + 2] = 0.5F;

    ret[COL_GRID * 3 + 0] = 0.0F;
    ret[COL_GRID * 3 + 1] = 0.0F;
    ret[COL_GRID * 3 + 2] = 0.0F;

    ret[COL_LINE * 3 + 0] = 0.1F;
    ret[COL_LINE * 3 + 1] = 0.1F;
    ret[COL_LINE * 3 + 2] = 0.1F;

    ret[COL_DRAGLINE * 3 + 0] = 0.0F;
    ret[COL_DRAGLINE * 3 + 1] = 0.0F;
    ret[COL_DRAGLINE * 3 + 2] = 1.0F;

    ret[COL_ERROR * 3 + 0] = 1.0F;
    ret[COL_ERROR * 3 + 1] = 0.0F;
    ret[COL_ERROR * 3 + 2] = 0.0F;

    *ncolours = NCOLOURS;
    return ret;
}

static game_drawstate *game_new_drawstate(drawing *dr, const game_state *state) {
    struct game_drawstate *ds = snew(struct game_drawstate);

    ds->tilesize = 0;
    ds->started = FALSE;

    return ds;
}

static void game_free_drawstate(drawing *dr, game_drawstate *ds) {
    sfree(ds);
}

static void draw_horizontal_dotted_line(drawing *dr, int x1, int x2, int y, int colour) {
    int i;
    for (i=x1;i<x2;i+=4) draw_line(dr, i, y, i+1, y, colour);
}
static void draw_vertical_dotted_line(drawing *dr, int y1, int y2, int x, int colour) {
    int i;
    for (i=y1;i<y2;i+=4) draw_line(dr, x, i, x, i+1, colour);
}

static void draw_square(drawing *dr, game_drawstate *ds, const game_ui *ui, int i, const game_state *state) {
    int x = i % (state->shared->w);
    int y = i / (state->shared->w);
    int width = ds->tilesize/6;
    int parity = (x % 2 == 0);
    if (y % 2 == 0) parity = !parity;
 
    draw_rect(dr, COORD(x), COORD(y), ds->tilesize, ds->tilesize, parity ? COL_FLOOR_A : COL_FLOOR_B);

    draw_horizontal_dotted_line(dr, COORD(x), COORD(x+1), COORD(y), COL_GRID);
    draw_horizontal_dotted_line(dr, COORD(x), COORD(x+1), COORD(y+1), COL_GRID);
    draw_vertical_dotted_line(  dr, COORD(y), COORD(y+1), COORD(x), COL_GRID);
    draw_vertical_dotted_line(  dr, COORD(y), COORD(y+1), COORD(x+1), COL_GRID);
 
    if (state->lines[i] & L)
        draw_rect(dr, COORD(x), COORD(y)+(ds->tilesize/2)-width/2, ds->tilesize/2 + width/2, width, COL_DRAGLINE); 
    if (state->lines[i] & R)
        draw_rect(dr, COORD(x)+(ds->tilesize/2)-width/2, COORD(y)+(ds->tilesize/2)-width/2, ds->tilesize/2 + width/2 + 1, width, COL_DRAGLINE); 
    if (state->lines[i] & U)
        draw_rect(dr, COORD(x)+(ds->tilesize/2)-width/2, COORD(y), width, ds->tilesize/2 + width/2, COL_DRAGLINE); 
    if (state->lines[i] & D)
        draw_rect(dr, COORD(x)+(ds->tilesize/2)-width/2, COORD(y)+(ds->tilesize/2)-width/2, width, ds->tilesize/2 + width/2 + 1, COL_DRAGLINE); 
 
 
    return;
}

static void draw_wall_outline(drawing *dr, game_drawstate *ds, const game_ui *ui, int i, const game_state *state) {
    int w = state->shared->w;
    int h = state->shared->h;
    int ws = (w+1)*h;
    int x = (i < ws) ? i % (w+1) : (i-ws) % w;
    int y = (i < ws) ? i / (w+1) : (i-ws) / w;
    int width = ds->tilesize/16;

    if (i < ws) {
        draw_rect(dr, COORD(x)-width/2, COORD(y)-width/2, width, ds->tilesize + width, state->shared->fixed[i] ? COL_FIXED : COL_WALL);
    }
    else {
        draw_rect(dr, COORD(x)-width/2, COORD(y)-width/2, ds->tilesize + width, width, state->shared->fixed[i] ? COL_FIXED : COL_WALL);
    }
    
    return;
}

static void game_redraw(drawing *dr, game_drawstate *ds,
                        const game_state *oldstate, const game_state *state,
                        int dir, const game_ui *ui,
                        float animtime, float flashtime) {
    int w = state->shared->w, h = state->shared->h;
    int i;
    
    if (!ds->started) {
        draw_rect(dr, 0, 0, w*TILESIZE + 2*BORDER, h*TILESIZE + 2*BORDER,
                    COL_BACKGROUND);
        ds->started = TRUE;
    }


    for (i=0;i<state->shared->wh;i++)
        draw_square(dr, ds, ui, i, state);

    for (i=0;i<state->shared->nw;i++)
        if (state->walls[i] && !state->shared->fixed[i])
            draw_wall_outline(dr, ds, ui, i, state);
            
    for (i=0;i<state->shared->nw;i++)
        if (state->walls[i] && state->shared->fixed[i])
            draw_wall_outline(dr, ds, ui, i, state);


    draw_update(dr, 0, 0, w*TILESIZE + 2*BORDER, h*TILESIZE + 2*BORDER);
    return;    
}

static float game_anim_length(const game_state *oldstate,
                              const game_state *newstate, int dir, game_ui *ui) {
    return 0.0F;
}

static float game_flash_length(const game_state *oldstate,
                               const game_state *newstate, int dir, game_ui *ui) {
    return 0.0F;
}

static int game_status(const game_state *state) {
    return 0;
}

static int game_timing_state(const game_state *state, game_ui *ui) {
    return TRUE;
}

static void game_print_size(const game_params *params, float *x, float *y) {
}

static void game_print(drawing *dr, const game_state *state, int tilesize) {
}

#ifdef COMBINED
#define thegame walls
#endif

const struct game thegame = {
    "Walls", "games.walls", "walls",
    default_params,
    game_fetch_preset,
    decode_params,
    encode_params,
    free_params,
    dup_params,
    TRUE, game_configure, custom_params,
    validate_params,
    new_game_desc,
    validate_desc,
    new_game,
    dup_game,
    free_game,
    TRUE, solve_game,
    TRUE, game_can_format_as_text_now, game_text_format,
    new_ui,
    free_ui,
    encode_ui,
    decode_ui,
    game_changed_state,
    interpret_move,
    execute_move,
    PREFERRED_TILE_SIZE, game_compute_size, game_set_size,
    game_colours,
    game_new_drawstate,
    game_free_drawstate,
    game_redraw,
    game_anim_length,
    game_flash_length,
    game_status,
    FALSE, FALSE, game_print_size, game_print,
    FALSE,			       /* wants_statusbar */
    FALSE, game_timing_state,
    0,				       /* flags */
};


#ifdef STANDALONE_SOLVER

#include <stdarg.h>

int main(int argc, char **argv) {
    game_params *p;
    random_state *rs;
    int c;
    rs = random_new("123456", 6);
    p = default_params();
    p->w = 6;
    p->h = 5;
    
    while (TRUE) {
        sfree(new_game_desc(p, rs, NULL, 0));
        c = getchar();
        if (c == 'q') break;
    }
    
    random_free(rs);
    free_params(p);
    return 0;
}

#endif
