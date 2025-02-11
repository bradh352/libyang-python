/*
 * Copyright (c) 2018-2022 Robin Jarry
 * SPDX-License-Identifier: MIT
 */

#include <libyang/libyang.h>
#include <libyang/version.h>

#if (LY_VERSION_MAJOR != 3)
#error "This version of libyang bindings only works with libyang 3.x"
#endif

typedef struct {
    char **results;
    size_t nresults;
    size_t alloc_results;
} pyly_string_list_t;

/*! Takes append an entry to a dynamic array of strings
 *  \param[in] l    Pointer to pyly_string_list_t object (must be initialized to zero)
 *  \param[in] str  String, the pointer will be owned by the list
 */
static void pyly_strlist_append(pyly_string_list_t *l, char *str /* Takes ownership */)
{
    if (l == NULL || str == NULL) {
        return;
    }

    if (l->nresults + 1 > l->alloc_results) {
        if (l->alloc_results == 0) {
            l->alloc_results = 1;
        } else {
            l->alloc_results <<= 1;
        }
        l->results = realloc(l->results, l->alloc_results * sizeof(*l->results));
    }
    l->results[l->nresults++] = str;
}

void pyly_cstr_array_free(char **list, size_t nlist)
{
    size_t i;

    if (list == NULL)
        return;

    for (i=0; i<nlist; i++) {
        free(list[i]);
    }
    free(list);
}

typedef struct {
    const struct ly_ctx *ctx;
    const char          *base_path;
    int                  include_children;
    pyly_string_list_t  *res;
} pyly_dfs_data_t;

static char *pyly_lref_to_xpath(const struct lysc_node *node, const struct lysc_type_leafref *lref)
{
    struct ly_set *set = NULL;
    LY_ERR         err;
    char          *path = NULL;

    err = lys_find_expr_atoms(node, node->module, lref->path, lref->prefixes, 0, &set);
    if (err != LY_SUCCESS) {
        return NULL;
    }

    if (set->count != 0) {
        path = lysc_path(set->snodes[set->count - 1], LYSC_PATH_DATA, NULL, 0);
    }

    ly_set_free(set, NULL);
    return path;
}

static size_t pyly_snode_fetch_leafrefs(const struct lysc_node *node, char ***out)
{
    pyly_string_list_t           res;
    const struct lysc_node_leaf *leaf;

    if (node == NULL || out == NULL) {
        return 0;
    }

    memset(&res, 0, sizeof(res));
    *out = NULL;

    /* Not a node type we are interested in */
    if (node->nodetype != LYS_LEAF && node->nodetype != LYS_LEAFLIST) {
        return 0;
    }

    leaf = (const struct lysc_node_leaf *)node;
    if (leaf->type->basetype == LY_TYPE_UNION) {
        /* Unions are a bit of a pain as they aren't represented by nodes,
         * so we need to iterate across them to see if they contain any
         * leafrefs */
        const struct lysc_type_union *un = (const struct lysc_type_union *)leaf->type;
        size_t                        i;

        for (i=0; i<LY_ARRAY_COUNT(un->types); i++) {
            const struct lysc_type *utype = un->types[i];

            if (utype->basetype != LY_TYPE_LEAFREF) {
                continue;
            }

            pyly_strlist_append(&res, pyly_lref_to_xpath(node, (const struct lysc_type_leafref *)utype));
        }
    } else if (leaf->type->basetype == LY_TYPE_LEAFREF) {
        const struct lysc_node *base_node = lysc_node_lref_target(node);

        if (base_node == NULL) {
            return 0;
        }

        pyly_strlist_append(&res, lysc_path(base_node, LYSC_PATH_DATA, NULL, 0));
    } else {
        /* Not a node type we're interested in */
        return 0;
    }

    *out = res.results;
    return res.nresults;
}

/*! For the given xpath, return the xpaths for the nodes they reference.
 *
 *  \param[in]  ctx       Initialized context with loaded schema
 *  \param[in]  xpath     Xpath
 *  \param[out] out       Pointer passed by reference that will hold a C array
 *                        of c strings representing the paths for any leaf
 *                        references.
 *  \return number of results, or 0 if none.
 */
size_t pyly_backlinks_xpath_leafrefs(const struct ly_ctx *ctx, const char *xpath, char ***out)
{
    LY_ERR             err;
    struct ly_set     *set = NULL;
    size_t             i;
    pyly_string_list_t res;

    if (ctx == NULL || xpath == NULL || out == NULL) {
        return 0;
    }

    memset(&res, 0, sizeof(res));

    *out = NULL;

    err = lys_find_xpath(ctx, NULL, xpath, 0, &set);
    if (err != LY_SUCCESS) {
        return 0;
    }

    for (i=0; i<set->count; i++) {
        size_t cnt;
        size_t j;
        char **refs = NULL;
        cnt = pyly_snode_fetch_leafrefs(set->snodes[i], &refs);
        for (j=0; j<cnt; j++) {
            pyly_strlist_append(&res, strdup(refs[j]));
        }
        pyly_cstr_array_free(refs, cnt);
    }

    ly_set_free(set, NULL);

    *out = res.results;
    return res.nresults;
}

static LY_ERR pyly_backlinks_find_leafref_nodes_clb(struct lysc_node *node, void *data, ly_bool *dfs_continue)
{
    pyly_dfs_data_t *dctx     = data;
    char           **leafrefs = NULL;
    size_t           nleafrefs;
    size_t           i;
    int              found    = 0;

    /* Not a node type we are interested in */
    if (node->nodetype != LYS_LEAF && node->nodetype != LYS_LEAFLIST) {
        return LY_SUCCESS;
    }

    /* Fetch leafrefs for comparison against our base path.  Even if we are
     * going to throw them away, we still need a count to know if this has
     * leafrefs */
    nleafrefs = pyly_snode_fetch_leafrefs(node, &leafrefs);
    if (nleafrefs == 0) {
        return LY_SUCCESS;
    }

    for (i=0; i<nleafrefs && !found; i++) {
        if (dctx->base_path != NULL) {
            if (dctx->include_children) {
                if (strncmp(leafrefs[i], dctx->base_path, strlen(dctx->base_path)) != 0) {
                    continue;
                }
            } else {
                if (strcmp(leafrefs[i], dctx->base_path) != 0) {
                    continue;
                }
            }
        }
        found = 1;
    }
    pyly_cstr_array_free(leafrefs, nleafrefs);

    if (found) {
        pyly_strlist_append(dctx->res, lysc_path(node, LYSC_PATH_DATA, NULL, 0));
    }

    return LY_SUCCESS;
}

/*! Search the entire loaded schema for any nodes that contain a leafref and
 *  record the path.  If a base_path is specified, only leafrefs that point to
 *  the specified path will be recorded, if include_children is 1, then children
 *  of the specified path are also included.
 *
 *  This function is used in replacement for the concept of backlink references
 *  that were part of libyang v1 but were subsequently removed.  This is
 *  implemented in C code due to the overhead involved with needing to produce
 *  Python nodes for results for evaluation which has a high overhead.
 *
 *  If building a data cache to more accurately replicate the prior backlinks
 *  concept, pass NULL to base_path which will then return any paths that
 *  reference another.  It is then the caller's responsibility to look up where
 *  the leafref is pointing as part of building the cache. It is expected most
 *  users will not need the cache and will simply pass in the base_path as needed.
 *
 *  \param[in]  ctx              Initialized context with loaded schema
 *  \param[in]  base_path        Optional base node path to restrict output.
 *  \param[in]  include_children Whether or not to include children of the
 *                               specified base path or if the path is an
 *                               explicit reference.
 *  \param[out] out              Pointer passed by reference that will hold a C
 *                               array of c strings representing the paths for
 *                               any leaf references.
 *  \return number of results, or 0 if none.
 */
size_t pyly_backlinks_find_leafref_nodes(const struct ly_ctx *ctx, const char *base_path, int include_children, char ***out)
{
    pyly_string_list_t       res;
    uint32_t                 module_idx = 0;
    const struct lys_module *module;

    memset(&res, 0, sizeof(res));

    if (ctx == NULL || out == NULL) {
        return 0;
    }

    /* Iterate across all loaded modules */
    for (module_idx = 0; (module = ly_ctx_get_module_iter(ctx, &module_idx)) != NULL; ) {
        pyly_dfs_data_t data = { ctx, base_path, include_children, &res };

        lysc_module_dfs_full(module, pyly_backlinks_find_leafref_nodes_clb, &data);
        /* Ignore error */
    }

    *out = res.results;
    return res.nresults;
}
