// =============================================================================================== //
// kutaQ3 hook tests - stand-in for glStateGuard.cpp
//
// GL::LegacyStateGuard is existing, unmodified repo code: it snapshots and restores Quake 3's
// legacy GL state around a block of overlay drawing. The test does not need that behaviour (there
// is no driver state to restore), it only needs the scoped wrapper nameEsp.cpp uses to exist and to
// be observable, so the calls it makes are recorded like everything else.
// =============================================================================================== //

#include "glStateGuard.h"
#include "glrec.h"

namespace GL
{
	bool LegacyStateGuard::IsActive()
	{
		return false;
	}

	LegacyStateGuard::LegacyStateGuard()
	{
		Rec::Push0("LegacyStateGuard()");
		Capture();
		Neutralise();
	}

	LegacyStateGuard::~LegacyStateGuard()
	{
		Restore();
		Rec::Push0("~LegacyStateGuard()");
	}

	void LegacyStateGuard::Capture()    {}
	void LegacyStateGuard::Neutralise() {}
	void LegacyStateGuard::Restore()    {}
}
