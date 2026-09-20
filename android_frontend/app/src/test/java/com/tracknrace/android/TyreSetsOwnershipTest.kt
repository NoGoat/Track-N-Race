package com.tracknrace.android

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TyreSetsOwnershipTest {
    @Test
    fun acceptsPlayersOwnRow() {
        assertTrue(ownsTyreSets(carIdx = 7, playerIdx = 7))
        assertTrue(ownsTyreSets(carIdx = 0, playerIdx = 0))
    }

    @Test
    fun rejectsOtherCarsRows() {
        assertFalse(ownsTyreSets(carIdx = 3, playerIdx = 7))
    }

    @Test
    fun rejectsTaggedRowUntilPlayerIsKnown() {
        assertFalse(ownsTyreSets(carIdx = 7, playerIdx = -1))
    }

    @Test
    fun acceptsUntaggedRowsFromOlderRecordingsAndDesktops() {
        assertTrue(ownsTyreSets(carIdx = null, playerIdx = -1))
        assertTrue(ownsTyreSets(carIdx = null, playerIdx = 7))
        assertTrue(ownsTyreSets(carIdx = -1, playerIdx = 7))
    }
}
