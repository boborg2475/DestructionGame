// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Uniquely named namespace, not anonymous: a unity build merges translation units. The
 * `using namespace` lives inside RunTest for the same reason.
 */
namespace PanelOffsetTestSupport
{
	/**
	 * A 1920 x 1080 viewport and today's 640 x 560 panel, transcribed rather than imported so the
	 * arithmetic is checkable by hand. Expectations are derived from these, not tied to them.
	 */
	constexpr double PanelViewportWidthPx = 1920.0;
	constexpr double PanelViewportHeightPx = 1080.0;
	constexpr double PanelWidthPx = 640.0;
	constexpr double PanelHeightPx = 560.0;

	/** Largest top-left corner that keeps the whole panel on screen: 1280 x 520 px. */
	constexpr double PanelMaxOffsetXPx = PanelViewportWidthPx - PanelWidthPx;
	constexpr double PanelMaxOffsetYPx = PanelViewportHeightPx - PanelHeightPx;

	/**
	 * A panel larger than the screen: 2000 px on 1920 px gives a range of -80 px, where a naive
	 * clamp misbehaves. Small windows and high DPI scales reach the same arithmetic.
	 */
	constexpr double PanelOversizeWidthPx = 2000.0;
	constexpr double PanelOversizeHeightPx = 1200.0;

	double PanelNaN()
	{
		return std::numeric_limits<double>::quiet_NaN();
	}

	double PanelInfinity()
	{
		return std::numeric_limits<double>::infinity();
	}

	FString DescribeOffset(const FVector2D& Offset)
	{
		return FString::Printf(TEXT("(%.4f, %.4f)"), Offset.X, Offset.Y);
	}

	bool OffsetIsFinite(const FVector2D& Offset)
	{
		return FMath::IsFinite(Offset.X) && FMath::IsFinite(Offset.Y);
	}

	/** One row: a drag, the panel and screen it happened on, and where the corner must end up. */
	struct FPanelOffsetCase
	{
		const TCHAR* Description;
		FVector2D DesiredOffsetPx;
		FVector2D PanelSizePx;
		FVector2D ViewportSizePx;
		FVector2D ExpectedOffsetPx;
	};

	/** Gap between the panel and the viewport's right edge, the old right-aligned 24 px margin. */
	constexpr double PanelHomeMarginPx = 24.0;

	/**
	 * A smaller panel, so the home must depend on panel size (compact mode). Not compact's real
	 * size; just different on both axes.
	 */
	constexpr double SmallPanelWidthPx = 448.0;
	constexpr double SmallPanelHeightPx = 392.0;

	/** One row: the panel and screen the menu opened on, and the corner it must open at. */
	struct FPanelHomeCase
	{
		const TCHAR* Description;
		FVector2D PanelSizePx;
		FVector2D ViewportSizePx;
		double MarginPx;
		FVector2D ExpectedOffsetPx;

		/**
		 * Whether the intended home fits on this screen. Such rows are also checked for being right
		 * of centre and vertically centred; the rest only promise an on-screen corner.
		 */
		bool bTheArguedHomeFits;
	};
}

/**
 * A dragged panel keeps its offset unless part of it would leave the screen; a panel larger
 * than the screen pins its top-left corner to the viewport origin, never a negative offset.
 * An off-screen title bar would leave the player unable to grab the panel again.
 *
 * Each row asserts the exact clamped corner, since containment alone passes a stub returning
 * the origin. Oversized rows come in pairs, dragged each way, because different clamp spellings
 * fail on different sides of an inverted range. Degenerate rows (NaN, infinity, negative sizes)
 * fail closed to the origin; a naive clamp turns them into plausible numbers. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPanelOffsetClampTest,
	"DestructionGame.Presenter.PanelOffsetClamp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPanelOffsetClampTest::RunTest(const FString& Parameters)
{
	using namespace PanelOffsetTestSupport;

	const FVector2D Screen(PanelViewportWidthPx, PanelViewportHeightPx);
	const FVector2D Panel(PanelWidthPx, PanelHeightPx);
	const FVector2D Origin(0.0, 0.0);

	// Furthest corner that keeps the panel wholly on screen.
	const FVector2D FarCorner(PanelMaxOffsetXPx, PanelMaxOffsetYPx);

	const TArray<FPanelOffsetCase> Cases = {
		{
			TEXT("a drag that leaves the whole panel on screen is honoured untouched"),
			FVector2D(100.0, 200.0), Panel, Screen,
			FVector2D(100.0, 200.0)
		},
		{
			TEXT("the panel's own default corner"),
			Origin, Panel, Screen,
			Origin
		},
		{
			// Exactly on the boundary, which is inside; catches > written for >=.
			TEXT("the far corner, where the panel's edges meet the screen's"),
			FarCorner, Panel, Screen,
			FarCorner
		},
		{
			TEXT("one pixel past the far corner comes back to it"),
			FVector2D(PanelMaxOffsetXPx + 1.0, PanelMaxOffsetYPx + 1.0), Panel, Screen,
			FarCorner
		},
		{
			// One axis at a time, so a clamp that couples the axes fails.
			TEXT("dragged off the right edge only, keeping its height"),
			FVector2D(1900.0, 200.0), Panel, Screen,
			FVector2D(PanelMaxOffsetXPx, 200.0)
		},
		{
			TEXT("dragged off the bottom edge only, keeping its position across"),
			FVector2D(100.0, 1000.0), Panel, Screen,
			FVector2D(100.0, PanelMaxOffsetYPx)
		},
		{
			TEXT("flung far past both edges"),
			FVector2D(5000.0, 5000.0), Panel, Screen,
			FarCorner
		},
		{
			TEXT("dragged off the top-left, past both zeroes"),
			FVector2D(-50.0, -50.0), Panel, Screen,
			Origin
		},
		{
			TEXT("off the left edge and off the bottom at once"),
			FVector2D(-50.0, 900.0), Panel, Screen,
			FVector2D(0.0, PanelMaxOffsetYPx)
		},
		{
			/*
			 * Inverted range (1920 - 2000 = -80), dragged right. Y is in range, so giving up on the
			 * whole vector also fails.
			 */
			TEXT("a panel wider than the screen, dragged right, pins its left edge"),
			FVector2D(300.0, 100.0),
			FVector2D(PanelOversizeWidthPx, PanelHeightPx), Screen,
			FVector2D(0.0, 100.0)
		},
		{
			// The same panel dragged left.
			TEXT("a panel wider than the screen, dragged left, pins its left edge too"),
			FVector2D(-300.0, 100.0),
			FVector2D(PanelOversizeWidthPx, PanelHeightPx), Screen,
			FVector2D(0.0, 100.0)
		},
		{
			TEXT("a panel taller than the screen, dragged down, pins its top edge"),
			FVector2D(100.0, 400.0),
			FVector2D(PanelWidthPx, PanelOversizeHeightPx), Screen,
			FVector2D(100.0, 0.0)
		},
		{
			TEXT("a panel taller than the screen, dragged up, pins its top edge too"),
			FVector2D(100.0, -400.0),
			FVector2D(PanelWidthPx, PanelOversizeHeightPx), Screen,
			FVector2D(100.0, 0.0)
		},
		{
			// A zero-wide range, the boundary of inversion.
			TEXT("a panel exactly the size of the screen has nowhere to go"),
			FVector2D(10.0, 10.0), Screen, Screen,
			Origin
		},
		{
			TEXT("a viewport of no size at all"),
			FVector2D(100.0, 100.0), Panel, FVector2D(0.0, 0.0),
			Origin
		},
		{
			TEXT("a drag whose X is not a number"),
			FVector2D(PanelNaN(), 200.0), Panel, Screen,
			Origin
		},
		{
			TEXT("a drag whose Y is not a number"),
			FVector2D(100.0, PanelNaN()), Panel, Screen,
			Origin
		},
		{
			TEXT("a viewport whose height is not a number"),
			FVector2D(100.0, 200.0), Panel, FVector2D(PanelViewportWidthPx, PanelNaN()),
			Origin
		},
		{
			TEXT("a panel whose width is not a number"),
			FVector2D(100.0, 200.0), FVector2D(PanelNaN(), PanelHeightPx), Screen,
			Origin
		},
		{
			TEXT("a drag flung to infinity"),
			FVector2D(PanelInfinity(), PanelInfinity()), Panel, Screen,
			Origin
		},
		{
			TEXT("a viewport of infinite width"),
			FVector2D(100.0, 200.0), Panel, FVector2D(PanelInfinity(), PanelViewportHeightPx),
			Origin
		},
		{
			// A negative size widens the permitted range (fail-open), so it is refused.
			TEXT("a panel of negative width"),
			FVector2D(100.0, 200.0), FVector2D(-PanelWidthPx, PanelHeightPx), Screen,
			Origin
		},
		{
			TEXT("a viewport of negative height"),
			FVector2D(100.0, 200.0), Panel, FVector2D(PanelViewportWidthPx, -PanelViewportHeightPx),
			Origin
		},
	};

	for (const FPanelOffsetCase& Case : Cases)
	{
		const FVector2D Clamped =
			ClampPanelOffset(Case.DesiredOffsetPx, Case.PanelSizePx, Case.ViewportSizePx);

		// Finiteness checked separately, so a NaN is reported as NaN rather than a wrong number.
		TestTrue(
			*FString::Printf(
				TEXT("%s: the clamped offset must be finite, it is %s"),
				Case.Description, *DescribeOffset(Clamped)),
			OffsetIsFinite(Clamped));

		TestEqual(
			FString::Printf(
				TEXT("%s: dragging to %s with a %s panel in a %s viewport should settle at %s, it settled at %s"),
				Case.Description,
				*DescribeOffset(Case.DesiredOffsetPx),
				*DescribeOffset(Case.PanelSizePx),
				*DescribeOffset(Case.ViewportSizePx),
				*DescribeOffset(Case.ExpectedOffsetPx),
				*DescribeOffset(Clamped)),
			Clamped, Case.ExpectedOffsetPx);

		// Swept invariant: the grabbable corner is always on screen.
		if (OffsetIsFinite(Clamped))
		{
			const double ScreenWidthPx = FMath::Max(Case.ViewportSizePx.X, 0.0);
			const double ScreenHeightPx = FMath::Max(Case.ViewportSizePx.Y, 0.0);

			TestTrue(
				*FString::Printf(
					TEXT("%s: the panel's top-left corner must stay on screen, %s is outside 0..%.1f x 0..%.1f"),
					Case.Description, *DescribeOffset(Clamped), ScreenWidthPx, ScreenHeightPx),
				Clamped.X >= 0.0 && Clamped.Y >= 0.0
					&& Clamped.X <= ScreenWidthPx && Clamped.Y <= ScreenHeightPx);
		}
	}

	// Clamping is idempotent; the output is fed back every frame, so drift would walk the panel.
	for (const FPanelOffsetCase& Case : Cases)
	{
		const FVector2D Once =
			ClampPanelOffset(Case.DesiredOffsetPx, Case.PanelSizePx, Case.ViewportSizePx);

		const FVector2D Twice = ClampPanelOffset(Once, Case.PanelSizePx, Case.ViewportSizePx);

		TestEqual(
			FString::Printf(
				TEXT("%s: clamping an already-clamped offset must change nothing, %s became %s"),
				Case.Description, *DescribeOffset(Once), *DescribeOffset(Twice)),
			Twice, Once);
	}

	return true;
}

/**
 * The panel opens right of centre and vertically centred, 24 px in from the right edge; on a
 * screen too small for that it opens at an on-screen corner, never a negative one. Regression:
 * making the panel draggable replaced right-alignment with an offset, which fell back to the
 * top-left. The home depends on panel size, since compact mode changes it.
 *
 * Each row asserts the exact corner; rows where the home fits are also checked for being right
 * of centre and centred. Clamping the home must change nothing, or the panel jumps when first
 * dragged. Degenerate rows add NaN, infinite and negative margins, all refused. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPanelHomeOffsetTest,
	"DestructionGame.Presenter.PanelHomeOffset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPanelHomeOffsetTest::RunTest(const FString& Parameters)
{
	using namespace PanelOffsetTestSupport;

	const FVector2D Screen(PanelViewportWidthPx, PanelViewportHeightPx);
	const FVector2D Panel(PanelWidthPx, PanelHeightPx);
	const FVector2D SmallPanel(SmallPanelWidthPx, SmallPanelHeightPx);
	const FVector2D Origin(0.0, 0.0);

	// Intended home on an ordinary screen: (1256, 260).
	const FVector2D ArguedHome(
		PanelViewportWidthPx - PanelWidthPx - PanelHomeMarginPx,
		(PanelViewportHeightPx - PanelHeightPx) * 0.5);

	const FVector2D SmallPanelHome(
		PanelViewportWidthPx - SmallPanelWidthPx - PanelHomeMarginPx,
		(PanelViewportHeightPx - SmallPanelHeightPx) * 0.5);

	const TArray<FPanelHomeCase> Cases = {
		{
			TEXT("the panel the game draws, on an ordinary screen"),
			Panel, Screen, PanelHomeMarginPx,
			ArguedHome, true
		},
		{
			// A smaller panel tucks further right; a hard-coded corner fails here.
			TEXT("a smaller panel, as a compact mode will ask for"),
			SmallPanel, Screen, PanelHomeMarginPx,
			SmallPanelHome, true
		},
		{
			// Zero margin lands exactly on the clamp's far corner; catches > written for >=.
			TEXT("no margin at all puts the panel flush against the right edge"),
			Panel, Screen, 0.0,
			FVector2D(PanelMaxOffsetXPx, (PanelViewportHeightPx - PanelHeightPx) * 0.5), false
		},
		{
			/*
			 * An oversized margin inverts the range (1920 - 640 - 2000 = -720). Y is ordinary, so
			 * giving up on the whole vector also fails.
			 */
			TEXT("a margin wider than the screen has room for pins the left edge"),
			Panel, Screen, 2000.0,
			FVector2D(0.0, (PanelViewportHeightPx - PanelHeightPx) * 0.5), false
		},
		{
			TEXT("a panel wider than the screen opens at the left edge, still centred down it"),
			FVector2D(PanelOversizeWidthPx, PanelHeightPx), Screen, PanelHomeMarginPx,
			FVector2D(0.0, (PanelViewportHeightPx - PanelHeightPx) * 0.5), false
		},
		{
			TEXT("a panel taller than the screen opens at the top edge, still tucked right"),
			FVector2D(PanelWidthPx, PanelOversizeHeightPx), Screen, PanelHomeMarginPx,
			FVector2D(PanelViewportWidthPx - PanelWidthPx - PanelHomeMarginPx, 0.0), false
		},
		{
			TEXT("a panel exactly the size of the screen has nowhere to be tucked to"),
			Panel, Panel, PanelHomeMarginPx,
			Origin, false
		},
		{
			// Smaller than the panel on both axes; must not go negative.
			TEXT("a viewport too small for the panel in both directions"),
			Panel, FVector2D(320.0, 240.0), PanelHomeMarginPx,
			Origin, false
		},
		{
			TEXT("a viewport of no size at all"),
			Panel, FVector2D(0.0, 0.0), PanelHomeMarginPx,
			Origin, false
		},
		{
			TEXT("a panel whose width is not a number"),
			FVector2D(PanelNaN(), PanelHeightPx), Screen, PanelHomeMarginPx,
			Origin, false
		},
		{
			TEXT("a panel whose height is not a number"),
			FVector2D(PanelWidthPx, PanelNaN()), Screen, PanelHomeMarginPx,
			Origin, false
		},
		{
			TEXT("a viewport whose width is not a number"),
			Panel, FVector2D(PanelNaN(), PanelViewportHeightPx), PanelHomeMarginPx,
			Origin, false
		},
		{
			TEXT("a viewport whose height is not a number"),
			Panel, FVector2D(PanelViewportWidthPx, PanelNaN()), PanelHomeMarginPx,
			Origin, false
		},
		{
			TEXT("a margin that is not a number"),
			Panel, Screen, PanelNaN(),
			Origin, false
		},
		{
			TEXT("a viewport of infinite width"),
			Panel, FVector2D(PanelInfinity(), PanelViewportHeightPx), PanelHomeMarginPx,
			Origin, false
		},
		{
			TEXT("a panel of infinite height"),
			FVector2D(PanelWidthPx, PanelInfinity()), Screen, PanelHomeMarginPx,
			Origin, false
		},
		{
			TEXT("an infinite margin"),
			Panel, Screen, PanelInfinity(),
			Origin, false
		},
		{
			// A negative size widens the range (fail-open).
			TEXT("a panel of negative width"),
			FVector2D(-PanelWidthPx, PanelHeightPx), Screen, PanelHomeMarginPx,
			Origin, false
		},
		{
			TEXT("a viewport of negative height"),
			Panel, FVector2D(PanelViewportWidthPx, -PanelViewportHeightPx), PanelHomeMarginPx,
			Origin, false
		},
		{
			/*
			 * A negative margin pushes the panel past the edge (1304 px), which the clamp would then
			 * silently hide. Refused.
			 */
			TEXT("a negative margin"),
			Panel, Screen, -PanelHomeMarginPx,
			Origin, false
		},
	};

	for (const FPanelHomeCase& Case : Cases)
	{
		const FVector2D Home =
			PieceMenuHomeOffset(Case.PanelSizePx, Case.ViewportSizePx, Case.MarginPx);

		// Finiteness checked separately, as for the clamp.
		TestTrue(
			*FString::Printf(
				TEXT("%s: the home offset must be finite, it is %s"),
				Case.Description, *DescribeOffset(Home)),
			OffsetIsFinite(Home));

		TestEqual(
			FString::Printf(
				TEXT("%s: a %s panel in a %s viewport with a %.1f px margin should open at %s, it opens at %s"),
				Case.Description,
				*DescribeOffset(Case.PanelSizePx),
				*DescribeOffset(Case.ViewportSizePx),
				Case.MarginPx,
				*DescribeOffset(Case.ExpectedOffsetPx),
				*DescribeOffset(Home)),
			Home, Case.ExpectedOffsetPx);

		// Swept invariant: the grabbable corner is on screen.
		if (OffsetIsFinite(Home))
		{
			const double ScreenWidthPx = FMath::Max(Case.ViewportSizePx.X, 0.0);
			const double ScreenHeightPx = FMath::Max(Case.ViewportSizePx.Y, 0.0);

			TestTrue(
				*FString::Printf(
					TEXT("%s: the panel must open somewhere on screen, %s is outside 0..%.1f x 0..%.1f"),
					Case.Description, *DescribeOffset(Home), ScreenWidthPx, ScreenHeightPx),
				Home.X >= 0.0 && Home.Y >= 0.0
					&& Home.X <= ScreenWidthPx && Home.Y <= ScreenHeightPx);
		}

		// The clamp must leave the home unchanged, or the panel jumps when first dragged.
		const FVector2D Clamped = ClampPanelOffset(Home, Case.PanelSizePx, Case.ViewportSizePx);

		TestEqual(
			FString::Printf(
				TEXT("%s: the home must already be a clamped offset, %s clamps to %s"),
				Case.Description, *DescribeOffset(Home), *DescribeOffset(Clamped)),
			Clamped, Home);

		// Where the home fits: right of centre (off the wall being aimed at) and vertically centred.
		if (!Case.bTheArguedHomeFits)
		{
			continue;
		}

		const double MiddleXPx = (Case.ViewportSizePx.X - Case.PanelSizePx.X) * 0.5;
		const double MiddleYPx = (Case.ViewportSizePx.Y - Case.PanelSizePx.Y) * 0.5;

		TestTrue(
			*FString::Printf(
				TEXT("%s: the panel must open RIGHT of centre, x %.2f is not past the middle at x %.2f"),
				Case.Description, Home.X, MiddleXPx),
			Home.X > MiddleXPx);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the panel must open CENTRED down the screen, y %.2f against a middle at y %.2f"),
				Case.Description, Home.Y, MiddleYPx),
			FMath::IsNearlyEqual(Home.Y, MiddleYPx, 0.5));

		// The whole panel is on screen.
		TestTrue(
			*FString::Printf(
				TEXT("%s: the whole panel must be on screen: it spans x %.2f..%.2f, y %.2f..%.2f in a %s viewport"),
				Case.Description,
				Home.X, Home.X + Case.PanelSizePx.X,
				Home.Y, Home.Y + Case.PanelSizePx.Y,
				*DescribeOffset(Case.ViewportSizePx)),
			Home.X + Case.PanelSizePx.X <= Case.ViewportSizePx.X
				&& Home.Y + Case.PanelSizePx.Y <= Case.ViewportSizePx.Y);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
