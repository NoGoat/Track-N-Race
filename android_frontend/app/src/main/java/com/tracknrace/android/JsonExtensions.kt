package com.tracknrace.android

import org.json.JSONObject

// libtnrp streams a V6 recording as field patches: a row carries only the
// values that were sampled, so "absent" means "unchanged", never zero. These
// return null for an absent or JSON-null field so callers can merge.

internal fun JSONObject.optionalInt(name: String): Int? =
    if (!has(name) || isNull(name)) null else optInt(name)

internal fun JSONObject.optionalDouble(name: String): Double? =
    if (!has(name) || isNull(name)) null else optDouble(name).takeUnless { it.isNaN() }

internal fun JSONObject.optionalBoolean(name: String): Boolean? =
    if (!has(name) || isNull(name)) null else optBoolean(name)
