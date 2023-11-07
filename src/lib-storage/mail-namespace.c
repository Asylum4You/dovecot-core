/* Copyright (c) 2005-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "file-lock.h"
#include "settings.h"
#include "mailbox-list-private.h"
#include "mail-storage-private.h"
#include "mail-storage-settings.h"
#include "mail-namespace.h"


static struct mail_namespace_settings prefixless_ns_set = {
	.name = "",
	.type = "private",
	.separator = "",
	.prefix = "",
	.location = "fail::LAYOUT=none",
	.unexpanded_location = "0fail::LAYOUT=none",
	.alias_for = "",

	.inbox = FALSE,
	.hidden = TRUE,
	.list = "no",
	.subscriptions = FALSE,
	.ignore_on_failure = FALSE,
	.disabled = FALSE,
};

static int
mail_namespaces_init_default_location(struct mail_user *user,
				      const char **error_r);

void mail_namespace_add_storage(struct mail_namespace *ns,
				struct mail_storage *storage)
{
	if (ns->storage == NULL)
		ns->storage = storage;
	array_push_back(&ns->all_storages, &storage);

	if (storage->v.add_list != NULL)
		storage->v.add_list(storage, ns->list);
	hook_mail_namespace_storage_added(ns);
}

void mail_namespace_finish_list_init(struct mail_namespace *ns,
				     struct mailbox_list *list)
{
	ns->list = list;
	ns->prefix_len = strlen(ns->prefix);
}

static void mail_namespace_free(struct mail_namespace *ns)
{
	struct mail_storage *storage;

	if (array_is_created(&ns->all_storages)) {
		array_foreach_elem(&ns->all_storages, storage)
			mail_storage_unref(&storage);
		array_free(&ns->all_storages);
	}
	if (ns->list != NULL)
		mailbox_list_destroy(&ns->list);

	if (ns->owner != ns->user && ns->owner != NULL)
		mail_user_unref(&ns->owner);
	if (ns->set->pool != NULL)
		settings_free(ns->set);
	i_free(ns->prefix);
	i_free(ns);
}

int mail_namespace_alloc(struct mail_user *user,
			 const struct mail_namespace_settings *ns_set,
			 struct mail_namespace **ns_r,
			 const char **error_r)
{
	struct mail_namespace *ns;

	ns = i_new(struct mail_namespace, 1);
	ns->refcount = 1;
	ns->user = user;
	ns->prefix = i_strdup(ns_set->prefix);
	ns->set = ns_set;
	if (ns_set->pool != NULL)
		pool_ref(ns_set->pool);
	i_array_init(&ns->all_storages, 2);

	if (strcmp(ns_set->type, "private") == 0) {
		ns->owner = user;
		ns->type = MAIL_NAMESPACE_TYPE_PRIVATE;
	} else if (strcmp(ns_set->type, "shared") == 0)
		ns->type = MAIL_NAMESPACE_TYPE_SHARED;
	else if (strcmp(ns_set->type, "public") == 0)
		ns->type = MAIL_NAMESPACE_TYPE_PUBLIC;
	else {
		*error_r = t_strdup_printf("Unknown namespace type: %s",
					   ns_set->type);
		mail_namespace_free(ns);
		return -1;
	}

	if (strcmp(ns_set->list, "children") == 0)
		ns->flags |= NAMESPACE_FLAG_LIST_CHILDREN;
	else if (strcmp(ns_set->list, "yes") == 0)
		ns->flags |= NAMESPACE_FLAG_LIST_PREFIX;
	else if (strcmp(ns_set->list, "no") != 0) {
		*error_r = t_strdup_printf("Invalid list setting value: %s",
					   ns_set->list);
		mail_namespace_free(ns);
		return -1;
	}

	if (ns_set->inbox) {
		ns->flags |= NAMESPACE_FLAG_INBOX_USER |
			NAMESPACE_FLAG_INBOX_ANY;
	}
	if (ns_set->hidden)
		ns->flags |= NAMESPACE_FLAG_HIDDEN;
	if (ns_set->subscriptions)
		ns->flags |= NAMESPACE_FLAG_SUBSCRIPTIONS;

	*ns_r = ns;

	return 0;
}

int mail_namespaces_init_add(struct mail_user *user,
			     const struct mail_namespace_settings *ns_set,
			     struct mail_namespace **ns_p, const char **error_r)
{
	const struct mail_storage_settings *mail_set =
		mail_user_set_get_storage_set(user);
	enum mail_storage_flags flags = 0;
	struct mail_namespace *ns;
	const char *error;
	int ret;

	if (*ns_set->location == '\0') {
		struct mail_namespace_settings *ns_set_copy =
			p_new(ns_set->pool, struct mail_namespace_settings, 1);
		*ns_set_copy = *ns_set;
		ns_set_copy->location = mail_set->mail_location;
		ns_set = ns_set_copy;
	}

	e_debug(user->event, "Namespace %s: type=%s, prefix=%s, sep=%s, "
		"inbox=%s, hidden=%s, list=%s, subscriptions=%s "
		"location=%s",
		ns_set->name, ns_set->type, ns_set->prefix,
		ns_set->separator == NULL ? "" : ns_set->separator,
		ns_set->inbox ? "yes" : "no",
		ns_set->hidden ? "yes" : "no",
		ns_set->list,
		ns_set->subscriptions ? "yes" : "no", ns_set->location);

	if ((ret = mail_namespace_alloc(user, ns_set, &ns, error_r)) < 0)
		return ret;

	if (ns_set == &prefixless_ns_set) {
		/* autocreated prefix="" namespace */
		ns->flags |= NAMESPACE_FLAG_UNUSABLE |
			NAMESPACE_FLAG_AUTOCREATED;
	}

	if (ns->type == MAIL_NAMESPACE_TYPE_SHARED &&
	    strchr(ns->prefix, '%') != NULL) {
		/* This is a dynamic shared namespace root under which new
		   per-user shared namespaces are created. The '%' is checked
		   to allow non-dynamic shared namespaces to be created with
		   explicit locations. */
		flags |= MAIL_STORAGE_FLAG_SHARED_DYNAMIC;
		ns->flags |= NAMESPACE_FLAG_NOQUOTA | NAMESPACE_FLAG_NOACL;
	}

	if (mail_storage_create(ns, flags, &error) < 0) {
		*error_r = t_strdup_printf("Namespace %s: %s",
					   ns->set->name, error);
		mail_namespace_free(ns);
		return -1;
	}

	*ns_p = ns;
	return 0;
}

static bool namespace_is_valid_alias_storage(struct mail_namespace *ns,
					     const char **error_r)
{
	if (strcmp(ns->storage->name, ns->alias_for->storage->name) != 0) {
		*error_r = t_strdup_printf(
			"Namespace %s can't have alias_for=%s "
			"to a different storage type (%s vs %s)",
			ns->set->name, ns->alias_for->prefix,
			ns->storage->name, ns->alias_for->storage->name);
		return FALSE;
	}

	if ((ns->storage->class_flags & MAIL_STORAGE_CLASS_FLAG_UNIQUE_ROOT) != 0 &&
	    ns->storage != ns->alias_for->storage) {
		*error_r = t_strdup_printf(
			"Namespace %s can't have alias_for=%s "
			"to a different storage (different root dirs)",
			ns->set->name, ns->alias_for->prefix);
		return FALSE;
	}
	return TRUE;
}

static int
namespace_set_alias_for(struct mail_namespace *ns,
			struct mail_namespace *all_namespaces,
			const char **error_r)
{
	if (ns->set->alias_for[0] != '\0') {
		ns->alias_for = mail_namespace_find_name(all_namespaces,
							 ns->set->alias_for);
		if (ns->alias_for == NULL) {
			*error_r = t_strdup_printf("Invalid namespace alias_for: %s",
						   ns->set->alias_for);
			return -1;
		}
		if (ns->alias_for->alias_for != NULL) {
			*error_r = t_strdup_printf("Chained namespace alias_for: %s",
						   ns->set->alias_for);
			return -1;
		}
		if (!namespace_is_valid_alias_storage(ns, error_r))
			return -1;

		if ((ns->alias_for->flags & NAMESPACE_FLAG_INBOX_USER) != 0) {
			/* copy inbox=yes */
			ns->flags |= NAMESPACE_FLAG_INBOX_USER;
		}

		ns->alias_chain_next = ns->alias_for->alias_chain_next;
		ns->alias_for->alias_chain_next = ns;
	}
	return 0;
}

static bool get_listindex_path(struct mail_namespace *ns, const char **path_r)
{
	const char *root;

	if (ns->list->set.list_index_fname[0] == '\0' ||
	    !mailbox_list_get_root_path(ns->list,
					MAILBOX_LIST_PATH_TYPE_LIST_INDEX,
					&root))
		return FALSE;

	*path_r = t_strconcat(root, "/", ns->list->set.list_index_fname, NULL);
	return TRUE;
}

static bool
namespace_has_duplicate_listindex(struct mail_namespace *ns,
				  const char **error_r)
{
	struct mail_namespace *ns2;
	const char *ns_list_index_path, *ns_mailboxes_root;
	const char *ns2_list_index_path, *ns2_mailboxes_root;

	if (!ns->list->mail_set->mailbox_list_index) {
		/* mailbox list indexes not in use */
		return FALSE;
	}

	if (!get_listindex_path(ns, &ns_list_index_path) ||
	    !mailbox_list_get_root_path(ns->list, MAILBOX_LIST_PATH_TYPE_MAILBOX,
					&ns_mailboxes_root))
		return FALSE;

	for (ns2 = ns->next; ns2 != NULL; ns2 = ns2->next) {
		if (!get_listindex_path(ns2, &ns2_list_index_path) ||
		    !mailbox_list_get_root_path(ns2->list, MAILBOX_LIST_PATH_TYPE_MAILBOX,
						&ns2_mailboxes_root))
			continue;

		if (strcmp(ns_list_index_path, ns2_list_index_path) == 0 &&
		    strcmp(ns_mailboxes_root, ns2_mailboxes_root) != 0) {
			*error_r = t_strdup_printf(
				"Namespaces %s and %s have different mailboxes paths, but duplicate LISTINDEX path. "
				"Add a unique LISTINDEX=<fname>",
				ns->set->name, ns2->set->name);
			return TRUE;
		}
	}
	return FALSE;
}

static bool
namespaces_check(struct mail_namespace *namespaces, const char **error_r)
{
	struct mail_namespace *ns, *inbox_ns = NULL;
	unsigned int subscriptions_count = 0;
	bool visible_namespaces = FALSE, have_list_yes = FALSE;
	char ns_sep, list_sep = '\0';

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		ns_sep = mail_namespace_get_sep(ns);
		if (mail_namespace_find_prefix(ns->next, ns->prefix) != NULL) {
			*error_r = t_strdup_printf(
				"Duplicate namespace prefix: \"%s\"",
				ns->prefix);
			return FALSE;
		}
		if ((ns->flags & NAMESPACE_FLAG_HIDDEN) == 0)
			visible_namespaces = TRUE;
		/* check the inbox=yes status before alias_for changes it */
		if ((ns->flags & NAMESPACE_FLAG_INBOX_USER) != 0) {
			if (inbox_ns != NULL) {
				*error_r = "There can be only one namespace with "
					"inbox=yes";
				return FALSE;
			}
			inbox_ns = ns;
		}
		if (namespace_set_alias_for(ns, namespaces, error_r) < 0)
			return FALSE;
		if (namespace_has_duplicate_listindex(ns, error_r))
			return FALSE;

		if (*ns->prefix != '\0' &&
		    (ns->flags & (NAMESPACE_FLAG_LIST_PREFIX |
				  NAMESPACE_FLAG_LIST_CHILDREN)) != 0 &&
		    ns->prefix[strlen(ns->prefix)-1] != ns_sep) {
			*error_r = t_strdup_printf(
				"list=yes requires prefix=%s "
				"to end with separator %c", ns->prefix, ns_sep);
			return FALSE;
		}
		if (*ns->prefix != '\0' &&
		    (ns->flags & (NAMESPACE_FLAG_LIST_PREFIX |
				  NAMESPACE_FLAG_LIST_CHILDREN)) != 0 &&
		    ns->prefix[0] == ns_sep) {
			*error_r = t_strdup_printf(
				"list=yes requires prefix=%s "
				"not to start with separator", ns->prefix);
			return FALSE;
		}
		if ((ns->flags & (NAMESPACE_FLAG_LIST_PREFIX |
				  NAMESPACE_FLAG_LIST_CHILDREN)) != 0) {
			if ((ns->flags & NAMESPACE_FLAG_LIST_PREFIX) != 0)
				have_list_yes = TRUE;
			if (list_sep == '\0')
				list_sep = ns_sep;
			else if (list_sep != ns_sep) {
				*error_r = "All list=yes namespaces must use "
					"the same separator";
				return FALSE;
			}
		}
		if ((ns->flags & NAMESPACE_FLAG_SUBSCRIPTIONS) != 0)
			subscriptions_count++;
	}

	if (inbox_ns == NULL) {
		*error_r = "inbox=yes namespace missing";
		return FALSE;
	}
	if (!have_list_yes) {
		*error_r = "list=yes namespace missing";
		return FALSE;
	}
	if (!visible_namespaces) {
		*error_r = "hidden=no namespace missing";
		return FALSE;
	}
	if (subscriptions_count == 0) {
		*error_r = "subscriptions=yes namespace missing";
		return FALSE;
	}
	return TRUE;
}

int mail_namespaces_init_finish(struct mail_namespace *namespaces,
				const char **error_r)
{
	struct mail_namespace *ns;
	bool prefixless_found = FALSE;

	i_assert(namespaces != NULL);

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		if (ns->prefix_len == 0)
			prefixless_found = TRUE;
	}
	if (!prefixless_found) {
		if (mail_namespaces_init_add(namespaces->user,
					     &prefixless_ns_set,
					     &ns, error_r) < 0)
			i_unreached();
		ns->next = namespaces;
		namespaces = ns;
	}
	if (namespaces->user->autocreated) {
		/* e.g. raw user - don't check namespaces' validity */
	} else if (!namespaces_check(namespaces, error_r)) {
		namespaces->user->error =
			t_strconcat("namespace configuration error: ",
				    *error_r, NULL);
	}

	if (namespaces->user->error == NULL) {
		mail_user_add_namespace(namespaces->user, &namespaces);
		T_BEGIN {
			hook_mail_namespaces_created(namespaces);
		} T_END;
	}

	/* allow namespace hooks to return failure via the user error */
	if (namespaces->user->error != NULL) {
		namespaces->user->namespaces = NULL;
		*error_r = t_strdup(namespaces->user->error);
		while (namespaces != NULL) {
			ns = namespaces;
			namespaces = ns->next;
			mail_namespace_free(ns);
		}
		return -1;
	}

	namespaces->user->namespaces_created = TRUE;
	return 0;
}

int mail_namespaces_init(struct mail_user *user, const char **error_r)
{
	const struct mail_namespace_settings *ns_set;
	const char *const *ns_names, *error;
	struct mail_namespace *namespaces, **ns_p;
	unsigned int i, count;

	i_assert(user->initialized);

        namespaces = NULL; ns_p = &namespaces;

	const struct mail_storage_settings *mail_set =
		mail_user_set_get_storage_set(user);
	if (array_is_created(&mail_set->namespaces))
		ns_names = array_get(&mail_set->namespaces, &count);
	else {
		ns_names = NULL;
		count = 0;
	}
	for (i = 0; i < count; i++) {
		if (settings_get_filter(user->event, "namespace", ns_names[i],
					&mail_namespace_setting_parser_info,
					0, &ns_set, &error) < 0) {
			*error_r = t_strdup_printf(
				"Failed to get namespace %s: %s",
				ns_names[i], error);
			return -1;
		}
		if (ns_set->disabled) {
			settings_free(ns_set);
			continue;
		}

		if (mail_namespaces_init_add(user, ns_set,
					     ns_p, error_r) < 0) {
			if (!ns_set->ignore_on_failure) {
				mail_namespaces_deinit(&namespaces);
				settings_free(ns_set);
				return -1;
			}
			e_debug(user->event, "Skipping namespace %s: %s",
				ns_set->prefix, *error_r);
		} else {
			ns_p = &(*ns_p)->next;
		}
		settings_free(ns_set);
	}

	if (namespaces == NULL) {
		/* no namespaces defined, create a default one */
		return mail_namespaces_init_default_location(user, error_r);
	}
	return mail_namespaces_init_finish(namespaces, error_r);
}

static int
mail_namespaces_init_location_full(struct mail_user *user, const char *location,
				   bool default_location, const char **error_r)
{
	struct mail_namespace_settings *inbox_set;
	struct mail_namespace *ns;
	int ret;

	inbox_set = p_new(user->pool, struct mail_namespace_settings, 1);
	*inbox_set = mail_namespace_default_settings;
	inbox_set->inbox = TRUE;
	/* enums must be changed */
	inbox_set->type = "private";
	inbox_set->list = "yes";
	inbox_set->location = p_strdup(user->pool, location);

	if (default_location) {
		/* treat this the same as if a namespace was created with
		   default settings. dsync relies on finding a namespace
		   without explicit location setting. */
		inbox_set->unexpanded_location = "";
	} else {
		inbox_set->unexpanded_location = inbox_set->location;
		inbox_set->unexpanded_location_override = TRUE;
	}

	if ((ret = mail_namespace_alloc(user, inbox_set, &ns, error_r)) < 0)
		return ret;

	if (mail_storage_create(ns, 0, error_r) < 0) {
		mail_namespace_free(ns);
		return -1;
	}
	return mail_namespaces_init_finish(ns, error_r);
}

static int
mail_namespaces_init_default_location(struct mail_user *user,
				      const char **error_r)
{
	const struct mail_storage_settings *mail_set;
	const char *location, *location_source, *error;
	bool default_location = FALSE;

	mail_set = mail_user_set_get_storage_set(user);
	if (*mail_set->mail_location != '\0') {
		location_source = "mail_location setting";
		location = mail_set->mail_location;
		default_location = TRUE;
	} else if ((location = getenv("MAIL")) != NULL) {
		location_source = "environment MAIL";
	} else if ((location = getenv("MAILDIR")) != NULL) {
		location = p_strdup_printf(user->pool, "maildir:%s", location);
		location_source = "environment MAILDIR";
	} else {
		location = "";
		location_source = "autodetection";
	}
	if (mail_namespaces_init_location_full(user, location,
					       default_location, &error) == 0)
		return 0;
	else if (location[0] != '\0') {
		*error_r = t_strdup_printf(
			"Initializing mail storage from %s failed: %s",
			location_source, error);
		return -1;
	} else {
		*error_r = t_strdup_printf("mail_location not set and "
					   "autodetection failed: %s", error);
		return -1;
	}
}

int mail_namespaces_init_location(struct mail_user *user, const char *location,
				  const char **error_r)
{
	return mail_namespaces_init_location_full(user, location,
						  FALSE, error_r);
}

struct mail_namespace *mail_namespaces_init_empty(struct mail_user *user)
{
	struct mail_namespace *ns;

	ns = i_new(struct mail_namespace, 1);
	ns->refcount = 1;
	ns->user = user;
	ns->owner = user;
	ns->prefix = i_strdup("");
	ns->flags = NAMESPACE_FLAG_INBOX_USER | NAMESPACE_FLAG_INBOX_ANY |
		NAMESPACE_FLAG_LIST_PREFIX | NAMESPACE_FLAG_SUBSCRIPTIONS;
	i_array_init(&ns->all_storages, 2);
	return ns;
}

void mail_namespaces_deinit(struct mail_namespace **_namespaces)
{
	struct mail_namespace *ns, *next;

	/* update *_namespaces as needed, instead of immediately setting it
	   to NULL. for example mdbox_storage.destroy() wants to go through
	   user's namespaces. */
	while (*_namespaces != NULL) {
		ns = *_namespaces;
		next = ns->next;

		mail_namespace_free(ns);
		*_namespaces = next;
	}
}

void mail_namespaces_set_storage_callbacks(struct mail_namespace *namespaces,
					   struct mail_storage_callbacks *callbacks,
					   void *context)
{
	struct mail_namespace *ns;
	struct mail_storage *storage;

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		array_foreach_elem(&ns->all_storages, storage)
			mail_storage_set_callbacks(storage, callbacks, context);
	}
}

void mail_namespace_ref(struct mail_namespace *ns)
{
	i_assert(ns->refcount > 0);

	ns->refcount++;
}

void mail_namespace_unref(struct mail_namespace **_ns)
{
	struct mail_namespace *ns = *_ns;

	i_assert(ns->refcount > 0);

	*_ns = NULL;

	if (--ns->refcount > 0)
		return;

	i_assert(ns->destroyed);
	mail_namespace_free(ns);
}

void mail_namespace_destroy(struct mail_namespace *ns)
{
	struct mail_namespace **nsp;

	i_assert(!ns->destroyed);

	/* remove from user's namespaces list */
	for (nsp = &ns->user->namespaces; *nsp != NULL; nsp = &(*nsp)->next) {
		if (*nsp == ns) {
			*nsp = ns->next;
			break;
		}
	}
	ns->destroyed = TRUE;

	mail_namespace_unref(&ns);
}

struct mail_storage *
mail_namespace_get_default_storage(struct mail_namespace *ns)
{
	return ns->storage;
}

char mail_namespace_get_sep(struct mail_namespace *ns)
{
	return *ns->set->separator != '\0' ? *ns->set->separator :
		mailbox_list_get_hierarchy_sep(ns->list);
}

char mail_namespaces_get_root_sep(struct mail_namespace *namespaces)
{
	while ((namespaces->flags & NAMESPACE_FLAG_LIST_PREFIX) == 0)
		namespaces = namespaces->next;
	return mail_namespace_get_sep(namespaces);
}

static bool mail_namespace_is_usable_prefix(struct mail_namespace *ns,
					    const char *mailbox, bool inbox)
{
	if (strncmp(ns->prefix, mailbox, ns->prefix_len) == 0) {
		/* true exact prefix match */
		return TRUE;
	}

	if (inbox && str_begins_with(ns->prefix, "INBOX") &&
	    strncmp(ns->prefix+5, mailbox+5, ns->prefix_len-5) == 0) {
		/* we already checked that mailbox begins with case-insensitive
		   INBOX. this namespace also begins with INBOX and the rest
		   of the prefix matches too. */
		return TRUE;
	}

	if (strncmp(ns->prefix, mailbox, ns->prefix_len-1) == 0 &&
	    mailbox[ns->prefix_len-1] == '\0' &&
	    ns->prefix[ns->prefix_len-1] == mail_namespace_get_sep(ns)) {
		/* we're trying to access the namespace prefix itself */
		return TRUE;
	}
	return FALSE;
}

static struct mail_namespace *
mail_namespace_find_mask(struct mail_namespace *namespaces, const char *box,
			 enum namespace_flags flags,
			 enum namespace_flags mask)
{
        struct mail_namespace *ns = namespaces;
	struct mail_namespace *best = NULL;
	size_t best_len = 0;
	const char *suffix;
	bool inbox;

	inbox = str_begins_icase(box, "INBOX", &suffix);
	if (inbox && suffix[0] == '\0') {
		/* find the INBOX namespace */
		while (ns != NULL) {
			if ((ns->flags & NAMESPACE_FLAG_INBOX_USER) != 0 &&
			    (ns->flags & mask) == flags)
				return ns;
			if (*ns->prefix == '\0')
				best = ns;
			ns = ns->next;
		}
		return best;
	}

	for (; ns != NULL; ns = ns->next) {
		if (ns->prefix_len >= best_len && (ns->flags & mask) == flags &&
		    mail_namespace_is_usable_prefix(ns, box, inbox)) {
			best = ns;
			best_len = ns->prefix_len;
		}
	}
	return best;
}

static struct mail_namespace *
mail_namespace_find_shared(struct mail_namespace *ns, const char *mailbox)
{
	struct mailbox_list *list = ns->list;
	struct mail_storage *storage;

	if (mailbox_list_get_storage(&list, &mailbox, 0, &storage) < 0)
		return ns;

	return mailbox_list_get_namespace(list);
}

struct mail_namespace *
mail_namespace_find(struct mail_namespace *namespaces, const char *mailbox)
{
	struct mail_namespace *ns;

	ns = mail_namespace_find_mask(namespaces, mailbox, 0, 0);
	i_assert(ns != NULL);

	if (mail_namespace_is_shared_user_root(ns)) {
		/* see if we need to autocreate a namespace for shared user */
		if (strchr(mailbox, mail_namespace_get_sep(ns)) != NULL)
			return mail_namespace_find_shared(ns, mailbox);
	}
	return ns;
}

struct mail_namespace *
mail_namespace_find_unalias(struct mail_namespace *namespaces,
			    const char **mailbox)
{
	struct mail_namespace *ns;
	const char *storage_name;

	ns = mail_namespace_find(namespaces, *mailbox);
	if (ns->alias_for != NULL) {
		storage_name =
			mailbox_list_get_storage_name(ns->list, *mailbox);
		ns = ns->alias_for;
		*mailbox = mailbox_list_get_vname(ns->list, storage_name);
	}
	return ns;
}

struct mail_namespace *
mail_namespace_find_visible(struct mail_namespace *namespaces,
			    const char *mailbox)
{
	return mail_namespace_find_mask(namespaces, mailbox, 0,
					NAMESPACE_FLAG_HIDDEN);
}

struct mail_namespace *
mail_namespace_find_subscribable(struct mail_namespace *namespaces,
				 const char *mailbox)
{
	return mail_namespace_find_mask(namespaces, mailbox,
					NAMESPACE_FLAG_SUBSCRIPTIONS,
					NAMESPACE_FLAG_SUBSCRIPTIONS);
}

struct mail_namespace *
mail_namespace_find_unsubscribable(struct mail_namespace *namespaces,
				   const char *mailbox)
{
	return mail_namespace_find_mask(namespaces, mailbox,
					0, NAMESPACE_FLAG_SUBSCRIPTIONS);
}

struct mail_namespace *
mail_namespace_find_inbox(struct mail_namespace *namespaces)
{
	i_assert(namespaces != NULL);

	/* there should always be an INBOX */
	while ((namespaces->flags & NAMESPACE_FLAG_INBOX_USER) == 0) {
		namespaces = namespaces->next;
		i_assert(namespaces != NULL);
	}
	return namespaces;
}

struct mail_namespace *
mail_namespace_find_prefix(struct mail_namespace *namespaces,
			   const char *prefix)
{
        struct mail_namespace *ns;
	size_t len = strlen(prefix);

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		if (ns->prefix_len == len &&
		    strcmp(ns->prefix, prefix) == 0)
			return ns;
	}
	return NULL;
}

struct mail_namespace *
mail_namespace_find_prefix_nosep(struct mail_namespace *namespaces,
				 const char *prefix)
{
        struct mail_namespace *ns;
	size_t len = strlen(prefix);

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		if (ns->prefix_len == len + 1 &&
		    strncmp(ns->prefix, prefix, len) == 0 &&
		    ns->prefix[len] == mail_namespace_get_sep(ns))
			return ns;
	}
	return NULL;
}

struct mail_namespace *
mail_namespace_find_name(struct mail_namespace *namespaces,
			 const char *name)
{
        struct mail_namespace *ns;

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		if (strcmp(ns->set->name, name) == 0)
			return ns;
	}
	return NULL;
}

bool mail_namespace_is_shared_user_root(struct mail_namespace *ns)
{
	struct mail_storage *storage;

	if (ns->type != MAIL_NAMESPACE_TYPE_SHARED)
		return FALSE;
	if ((ns->flags & NAMESPACE_FLAG_AUTOCREATED) != 0) {
		/* child of the shared root */
		return FALSE;
	}
	/* if we have driver=shared storage, we're a real shared root */
	array_foreach_elem(&ns->all_storages, storage) {
		if (strcmp(storage->name, MAIL_SHARED_STORAGE_NAME) == 0)
			return TRUE;
	}
	return FALSE;
}
