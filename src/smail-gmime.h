/* smail-gmime.h — GMime with g_autoptr() support */
#pragma once

#include <gmime/gmime.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GMimeStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GMimeParser, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GMimeMessage, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GMimeDataWrapper, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (InternetAddressList, g_object_unref)
