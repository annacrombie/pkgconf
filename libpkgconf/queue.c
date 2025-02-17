/*
 * queue.c
 * compilation of a list of packages into a world dependency set
 *
 * Copyright (c) 2012 pkgconf authors (see AUTHORS).
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * This software is provided 'as is' and without any warranty, express or
 * implied.  In no event shall the authors be liable for any damages arising
 * from the use of this software.
 */

#include <libpkgconf/stdinc.h>
#include <libpkgconf/libpkgconf.h>

/*
 * !doc
 *
 * libpkgconf `queue` module
 * =========================
 *
 * The `queue` module provides an interface that allows easily building a dependency graph from an
 * arbitrary set of dependencies.  It also provides support for doing "preflight" checks on the entire
 * dependency graph prior to working with it.
 *
 * Using the `queue` module functions is the recommended way of working with dependency graphs.
 */

typedef struct {
	pkgconf_node_t iter;
	char *package;
} pkgconf_queue_t;

/*
 * !doc
 *
 * .. c:function:: void pkgconf_queue_push(pkgconf_list_t *list, const char *package)
 *
 *    Pushes a requested dependency onto the dependency resolver's queue.
 *
 *    :param pkgconf_list_t* list: the dependency resolution queue to add the package request to.
 *    :param char* package: the dependency atom requested
 *    :return: nothing
 */
void
pkgconf_queue_push(pkgconf_list_t *list, const char *package)
{
	pkgconf_queue_t *pkgq = calloc(sizeof(pkgconf_queue_t), 1);

	pkgq->package = strdup(package);
	pkgconf_node_insert(&pkgq->iter, pkgq, list);
}

/*
 * !doc
 *
 * .. c:function:: bool pkgconf_queue_compile(pkgconf_client_t *client, pkgconf_pkg_t *world, pkgconf_list_t *list)
 *
 *    Compile a dependency resolution queue into a dependency resolution problem if possible, otherwise report an error.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to use for dependency resolution.
 *    :param pkgconf_pkg_t* world: The designated root of the dependency graph.
 *    :param pkgconf_list_t* list: The list of dependency requests to consider.
 *    :return: true if the built dependency resolution problem is consistent, else false
 *    :rtype: bool
 */
bool
pkgconf_queue_compile(pkgconf_client_t *client, pkgconf_pkg_t *world, pkgconf_list_t *list)
{
	pkgconf_node_t *iter;

	PKGCONF_FOREACH_LIST_ENTRY(list->head, iter)
	{
		pkgconf_queue_t *pkgq;

		pkgq = iter->data;
		pkgconf_dependency_parse(client, world, &world->required, pkgq->package, 0);
	}

	return (world->required.head != NULL);
}

/*
 * !doc
 *
 * .. c:function:: void pkgconf_queue_free(pkgconf_list_t *list)
 *
 *    Release any memory related to a dependency resolution queue.
 *
 *    :param pkgconf_list_t* list: The dependency resolution queue to release.
 *    :return: nothing
 */
void
pkgconf_queue_free(pkgconf_list_t *list)
{
	pkgconf_node_t *node, *tnode;

	PKGCONF_FOREACH_LIST_ENTRY_SAFE(list->head, tnode, node)
	{
		pkgconf_queue_t *pkgq = node->data;

		free(pkgq->package);
		free(pkgq);
	}
}

static void
pkgconf_queue_collect_dependents(pkgconf_client_t *client, pkgconf_pkg_t *pkg, void *data)
{
	pkgconf_node_t *node;
	pkgconf_pkg_t *world = data;

	if (pkg == world)
		return;

	PKGCONF_FOREACH_LIST_ENTRY(pkg->required.head, node)
	{
		pkgconf_dependency_t *flattened_dep;

		flattened_dep = pkgconf_dependency_copy(client, node->data);

		pkgconf_node_insert(&flattened_dep->iter, flattened_dep, &world->required);
	}

	PKGCONF_FOREACH_LIST_ENTRY(pkg->requires_private.head, node)
	{
		pkgconf_dependency_t *flattened_dep;

		flattened_dep = pkgconf_dependency_copy(client, node->data);

		pkgconf_node_insert(&flattened_dep->iter, flattened_dep, &world->requires_private);
	}
}

static int
dep_sort_cmp(const void *a, const void *b)
{
	const pkgconf_dependency_t *depA = *(void **) a;
	const pkgconf_dependency_t *depB = *(void **) b;

	return depB->match->hits - depA->match->hits;
}

static inline void
flatten_dependency_set(pkgconf_client_t *client, pkgconf_list_t *list)
{
	pkgconf_node_t *node;
	pkgconf_dependency_t **deps = NULL;
	size_t dep_count = 0, i;

	PKGCONF_FOREACH_LIST_ENTRY(list->head, node)
	{
		pkgconf_dependency_t *dep = node->data;
		pkgconf_pkg_t *pkg = pkgconf_pkg_verify_dependency(client, dep, NULL);

		if (pkg == NULL)
			continue;

		if (pkg->serial == client->serial)
			continue;

		if (dep->match == NULL)
		{
			PKGCONF_TRACE(client, "WTF: unmatched dependency %p <%s>", dep, dep->package);
			abort();
		}

		/* for virtuals, we need to check to see if there are dupes */
		for (i = 0; i < dep_count; i++)
		{
			pkgconf_dependency_t *other_dep = deps[i];

			PKGCONF_TRACE(client, "dedup %s = %s?", dep->package, other_dep->package);

			if (!strcmp(dep->package, other_dep->package))
			{
				PKGCONF_TRACE(client, "skipping, %zu deps", dep_count);
				goto next;
			}
		}

		pkg->serial = client->serial;

		/* copy to the deps table */
		dep_count++;
		deps = pkgconf_reallocarray(deps, dep_count, sizeof (void *));
		deps[dep_count - 1] = dep;

		PKGCONF_TRACE(client, "added %s to dep table", dep->package);

next:;
	}

	qsort(deps, dep_count, sizeof (void *), dep_sort_cmp);

	/* zero the list and start readding */
	pkgconf_list_zero(list);

	for (i = 0; i < dep_count; i++)
	{
		pkgconf_dependency_t *dep = deps[i];

		memset(&dep->iter, '\0', sizeof (dep->iter));
		pkgconf_node_insert(&dep->iter, dep, list);

		PKGCONF_TRACE(client, "slot %zu: dep %s matched to %p<%s> hits %lu", i, dep->package, dep->match, dep->match == NULL ? "NULL" : dep->match->id, dep->match->hits);
	}

	free(deps);
}

static inline unsigned int
pkgconf_queue_verify(pkgconf_client_t *client, pkgconf_pkg_t *world, pkgconf_list_t *list, int maxdepth)
{
	unsigned int result;

	if (!pkgconf_queue_compile(client, world, list))
		return PKGCONF_PKG_ERRF_DEPGRAPH_BREAK;

	/* collect all the dependencies */
	result = pkgconf_pkg_traverse(client, world, pkgconf_queue_collect_dependents, world, maxdepth, 0);
	if (result != PKGCONF_PKG_ERRF_OK)
		return result;

	/* flatten the dependency set using serials.
	 * we copy the dependencies to a vector, and then erase the list.
	 * then we copy them back to the list.
	 */
	++client->serial;

	PKGCONF_TRACE(client, "flattening requires deps");
	flatten_dependency_set(client, &world->required);

	++client->serial;

	PKGCONF_TRACE(client, "flattening requires.private deps");
	flatten_dependency_set(client, &world->requires_private);

	return PKGCONF_PKG_ERRF_OK;
}

/*
 * !doc
 *
 * .. c:function:: void pkgconf_queue_apply(pkgconf_client_t *client, pkgconf_list_t *list, pkgconf_queue_apply_func_t func, int maxdepth, void *data)
 *
 *    Attempt to compile a dependency resolution queue into a dependency resolution problem, then attempt to solve the problem and
 *    feed the solution to a callback function if a complete dependency graph is found.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to use for dependency resolution.
 *    :param pkgconf_list_t* list: The list of dependency requests to consider.
 *    :param pkgconf_queue_apply_func_t func: The callback function to call if a solution is found by the dependency resolver.
 *    :param int maxdepth: The maximum allowed depth for the dependency resolver.  A depth of -1 means unlimited.
 *    :param void* data: An opaque pointer which is passed to the callback function.
 *    :returns: true if the dependency resolver found a solution, otherwise false.
 *    :rtype: bool
 */
bool
pkgconf_queue_apply(pkgconf_client_t *client, pkgconf_list_t *list, pkgconf_queue_apply_func_t func, int maxdepth, void *data)
{
	pkgconf_pkg_t world = {
		.id = "virtual:world",
		.realname = "virtual world package",
		.flags = PKGCONF_PKG_PROPF_STATIC | PKGCONF_PKG_PROPF_VIRTUAL,
	};

	/* if maxdepth is one, then we will not traverse deeper than our virtual package. */
	if (!maxdepth)
		maxdepth = -1;

	if (pkgconf_queue_verify(client, &world, list, maxdepth) != PKGCONF_PKG_ERRF_OK)
		return false;

	/* the world dependency set is flattened after it is returned from pkgconf_queue_verify */
	if (!func(client, &world, data, maxdepth))
	{
		pkgconf_pkg_free(client, &world);
		return false;
	}

	pkgconf_pkg_free(client, &world);

	return true;
}

/*
 * !doc
 *
 * .. c:function:: void pkgconf_queue_validate(pkgconf_client_t *client, pkgconf_list_t *list, pkgconf_queue_apply_func_t func, int maxdepth, void *data)
 *
 *    Attempt to compile a dependency resolution queue into a dependency resolution problem, then attempt to solve the problem.
 *
 *    :param pkgconf_client_t* client: The pkgconf client object to use for dependency resolution.
 *    :param pkgconf_list_t* list: The list of dependency requests to consider.
 *    :param int maxdepth: The maximum allowed depth for the dependency resolver.  A depth of -1 means unlimited.
 *    :returns: true if the dependency resolver found a solution, otherwise false.
 *    :rtype: bool
 */
bool
pkgconf_queue_validate(pkgconf_client_t *client, pkgconf_list_t *list, int maxdepth)
{
	bool retval = true;
	pkgconf_pkg_t world = {
		.id = "virtual:world",
		.realname = "virtual world package",
		.flags = PKGCONF_PKG_PROPF_STATIC | PKGCONF_PKG_PROPF_VIRTUAL,
	};

	/* if maxdepth is one, then we will not traverse deeper than our virtual package. */
	if (!maxdepth)
		maxdepth = -1;

	if (pkgconf_queue_verify(client, &world, list, maxdepth) != PKGCONF_PKG_ERRF_OK)
		retval = false;

	pkgconf_pkg_free(client, &world);

	return retval;
}
