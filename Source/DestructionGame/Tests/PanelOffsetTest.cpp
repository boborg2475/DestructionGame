// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many
 * files into one. See CURRENT_STATE.md; the `using namespace` lives inside RunTest for the same
 * reason.
 */
namespace PanelOffsetTestSupport
{
	/**
	 * AN ORDINARY VIEWPORT AND THE PANEL THE GAME ACTUALLY DRAWS, BOTH TRANSCRIBED RATHER THAN
	 * IMPORTED.
	 *
	 * PieceMenuPanelWidthPx and PieceMenuPanelHeightPx are file-static in
	 * DestructionGamePlayerController.cpp and a test may not reach them — which is the right way
	 * round. A test that took the panel's size from the code that draws it would agree with that
	 * code whatever it said, including a size that no longer fits; these are the numbers the
	 * panel is 640 x 560 px today, written down so that the arithmetic below is arithmetic a
	 * human can check against a 1920 x 1080 screen.
	 *
	 * NOTHING BELOW DEPENDS ON THESE BEING THE SHIPPED FIGURES. Every expectation is derived from
	 * them in the case table, so a retuned panel moves the expectations with it.
	 */
	constexpr double PanelViewportWidthPx = 1920.0;
	constexpr double PanelViewportHeightPx = 1080.0;
	constexpr double PanelWidthPx = 640.0;
	constexpr double PanelHeightPx = 560.0;

	/**
	 * The largest top-left corner that still leaves the whole panel on screen: 1280 x 520 px.
	 *
	 * SPELLED OUT AS A SUBTRACTION RATHER THAN AS 1280, so the two numbers a reader has to hold
	 * are the panel and the screen. It is also the boundary every interesting row of the table
	 * below sits exactly on, one pixel inside, or one pixel outside.
	 */
	constexpr double PanelMaxOffsetXPx = PanelViewportWidthPx - PanelWidthPx;
	constexpr double PanelMaxOffsetYPx = PanelViewportHeightPx - PanelHeightPx;

	/**
	 * A PANEL WIDER THAN THE SCREEN IT IS ON, WHICH IS THE ROW THE WHOLE FILE IS FOR.
	 *
	 * 2000 px against a 1920 px viewport makes the permitted range -80 px, and a range whose top
	 * is below its bottom is where FMath::Clamp stops meaning what it reads as. It is not a silly
	 * input either: a 640 px panel on a 640 px-wide window is the same arithmetic, and so is any
	 * DPI scale that outruns the viewport.
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

	/**
	 * HOW FAR IN FROM THE VIEWPORT'S RIGHT EDGE THE PANEL OPENS, AND IT IS THE OLD FIGURE.
	 *
	 * The panel used to be an SBox at HAlign_Right / VAlign_Center with a 24 px margin, and that
	 * placement was argued on the record rather than defaulted into: a readout pinned against the
	 * right edge is off the wall the player is pointing at. PieceMenuPanelMarginPx was deleted along
	 * with the alignment when the panel became draggable, and this is the same 24 px coming back —
	 * as an ARGUMENT rather than as a constant beside the Slate, because that is the whole point of
	 * asking the presenter where the panel opens.
	 */
	constexpr double PanelHomeMarginPx = 24.0;

	/**
	 * A PANEL SMALLER THAN THE ONE THE GAME DRAWS TODAY, WHICH IS THE ROW THAT TIES THIS TO COMPACT
	 * MODE.
	 *
	 * PieceMenuPanelSizePx is about to answer a different size per detail mode, so the home has to
	 * be a function of the panel it is placing rather than a corner written down once — otherwise
	 * the compact panel opens at the full panel's corner and stands 192 px short of the right edge
	 * it was supposed to be tucked against. These are NOT a claim about what compact measures; they
	 * are simply a different size, chosen so the arithmetic changes on both axes.
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
		 * Whether this row is a screen the argued home actually FITS on.
		 *
		 * The rows where it does are held to the shape of the decision as well as to their own
		 * expected numbers — right of the screen's middle, and centred down it — so a mistyped
		 * expectation is still caught. The rows where it does not are the fail-closed ones, and the
		 * only thing they promise is a corner that is on screen at all.
		 */
		bool bTheArguedHomeFits;
	};
}

/**
 * A PANEL THE PLAYER HAS DRAGGED KEEPS THE OFFSET THEY CHOSE, EXCEPT WHERE THAT WOULD PUT ANY
 * PART OF IT OFF THE SCREEN — AND A PANEL TOO BIG FOR THE SCREEN PINS ITS TOP-LEFT CORNER TO THE
 * TOP-LEFT OF THE VIEWPORT RATHER THAN TO A NEGATIVE NUMBER.
 *
 * WHY THIS IS THE TESTABLE HALF. The player's complaint is "I can't move the menu if it is in my
 * way": the panel is pinned by an SConstraintCanvas with a fixed alignment and there is no way to
 * reposition it. Making it draggable is two parts — Slate reporting a drag, and deciding what
 * offset that drag means — and only the second one can be wrong in a way nobody notices. It is
 * also the only part that can strand the panel: an offset that puts the title bar off the top of
 * the screen leaves the player with no way to grab it again, and no way to get the panel back
 * short of restarting.
 *
 * THE ASSERTION IS THE CLAMPED OFFSET, EXACTLY, RATHER THAN "IT IS ON SCREEN SOMEWHERE". A
 * containment predicate is satisfied by a function that ignores its input and returns the origin,
 * which is the stub this test is red against — so containment alone would go green on a panel
 * that snapped back to the corner on every drag. Each row states the corner it must come back
 * with, and the containment invariant is swept over every row on top of that.
 *
 * THE OVERSIZED ROWS COME IN PAIRS, ONE DRAGGED EACH WAY, BECAUSE THE SPELLINGS THAT GET IT WRONG
 * GET IT WRONG DIFFERENTLY. With the permitted range at -80 px, Min(Max(X, 0), Range) hands back
 * -80 whichever way the panel was dragged, while a hand-written `X > Range ? Range : (X < 0 ? 0 :
 * X)` hands back -80 for a drag right and 0 for a drag left — so a single row would be passed by
 * one of the two. UE's own FMath::Clamp is Max(Min(X, Range), 0) and gets both right, which is
 * luck rather than a guarantee: measured against the naive Clamp these four rows are green, and
 * they are here so that the next spelling of the same line cannot quietly be the other one.
 *
 * THE DEGENERATE ROWS ARE WHERE THE NAIVE CLAMP ACTUALLY BREAKS, AND IT BREAKS EIGHT WAYS. NaN
 * does not survive Max and Min as a fault — Max discards it and Min replaces it — so a drag whose
 * X is not a number comes back as 1280.00 px, an infinite one as the far corner, and a viewport
 * of negative height as an ordinary offset a hundred pixels in. Every one of those is a
 * plausible-looking number standing in for "we do not know where the screen is". Each row also
 * asserts the result is finite separately from its value, so a NaN coming out is reported as a
 * NaN rather than as a mismatched number.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. Six doubles in, two out.
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

	/* The far corner the panel may be dragged to and still be wholly on screen. */
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
			/*
			 * EXACTLY ON THE BOUNDARY, WHICH MUST BE INSIDE IT. The panel's right edge lands on the
			 * viewport's right edge and not a pixel past, so a guard written with > where >= was
			 * meant would take a pixel off the range and nobody would ever see it.
			 */
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
			/*
			 * ONE AXIS AT A TIME, BOTH WAYS ROUND. A clamp that folded the two axes together —
			 * pulling the whole offset back when either edge overran — would pass a test that only
			 * ever pushed both at once, and it would make the panel jump vertically while the
			 * player dragged it along the bottom of the screen.
			 */
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
			 * THE INVERTED-RANGE PAIR, DRAGGED RIGHT. Viewport 1920 minus panel 2000 is -80, and
			 * a clamp nested the other way round hands back -80 — the panel's heading off the left
			 * edge of the screen, which is the one corner that must stay reachable. Y is an
			 * ordinary in-range drag on the same call, so an implementation that gave up on the
			 * whole vector when one axis was impossible is caught here too.
			 */
			TEXT("a panel wider than the screen, dragged right, pins its left edge"),
			FVector2D(300.0, 100.0),
			FVector2D(PanelOversizeWidthPx, PanelHeightPx), Screen,
			FVector2D(0.0, 100.0)
		},
		{
			/* And the same panel dragged the other way, which the naive clamp gets RIGHT. */
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
			/* Exactly the viewport is a zero-wide range, which is the boundary of the inversion. */
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
			/*
			 * A NEGATIVE SIZE IS NOT A SMALL PANEL. Subtracting one WIDENS the permitted range, so
			 * a viewport of -1920 px would let the corner be dragged further out than a real screen
			 * ever allows — the fail-OPEN direction, reached by arithmetic that looks perfectly
			 * reasonable on the line it is written.
			 */
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

		/*
		 * FINITE FIRST, AND SEPARATELY FROM THE VALUE. A NaN compares false against everything
		 * including itself, so an equality check alone would report "expected (0,0), got (nan,nan)"
		 * as an ordinary wrong number — this says which of the two faults it is.
		 */
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

		/*
		 * AND THE INVARIANT THE WHOLE FUNCTION EXISTS FOR, SWEPT OVER EVERY ROW RATHER THAN
		 * WRITTEN PER CASE: the corner the player has to be able to grab is never off the screen,
		 * in either direction. A row whose expectation was mistyped would still be held to this.
		 */
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

	/*
	 * AND CLAMPING IS IDEMPOTENT, WHICH IS WHAT MAKES A DRAG SURVIVE A SECOND FRAME. The offset
	 * this returns is stored and fed back in on the next drag, on the next resize and on every
	 * rebuild of the panel; a clamp that moved its own output would walk the panel across the
	 * screen a little further every frame it was asked. Swept over the same rows rather than
	 * written as its own case, because the rows that would show it are the clamped ones.
	 */
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
 * THE PANEL OPENS RIGHT-OF-CENTRE AND VERTICALLY CENTRED — AND ON A SCREEN TOO SMALL TO PUT IT
 * THERE IT OPENS AT A CORNER THAT IS STILL ON SCREEN, NEVER AT A NEGATIVE ONE.
 *
 * THIS IS A SELF-REPORTED REGRESSION AND THE CAUSE IS HONEST. The old placement was an SBox at
 * HAlign_Right / VAlign_Center with a 24 px margin, argued on the record as keeping the readout off
 * the wall the player is pointing at. Making the panel draggable replaced that alignment with an
 * OFFSET, and every corner except the origin is a function of the viewport's size — which nothing
 * knows until something has been laid out. So the arithmetic that used to run inside Slate's layout
 * now has to run before it, and the panel fell back to the one corner that needs no arithmetic at
 * all: the top-left. That is a corner nobody chose, over the part of the screen a player is most
 * likely to be looking at.
 *
 * WHY IT IS A FUNCTION HERE RATHER THAN TWO LINES AT THE CALL SITE. It is the same argument
 * ClampPanelOffset is here for, one step earlier: the numbers are viewport pixels, the failure is a
 * panel that opens somewhere unusable, and a widget cannot be asked whether it got them right. It
 * is also about to become a function of something that MOVES — PieceMenuPanelSizePx answers a
 * different size per detail mode, so the corner a compact panel opens at is not the corner a full
 * one does, and a home written as a constant would tuck one of the two against nothing.
 *
 * THE ASSERTION IS THE EXACT CORNER, PLUS THE SHAPE OF THE DECISION SWEPT OVER EVERY ROW THAT CAN
 * HOLD IT. "Somewhere on screen" is satisfied by the origin, which is the regression; "1256, 260"
 * alone is satisfied by a function that ignores its arguments on every other screen. So each row
 * states its corner, and the rows where the argued home fits are additionally held to being right
 * of the screen's own middle and centred down it — which is what a mistyped expectation would
 * still fail.
 *
 * AND IT COMPOSES WITH THE CLAMP RATHER THAN RESTATING IT, WHICH IS SWEPT OVER EVERY ROW. The home
 * is fed into the same field a drag writes to, and that field is only ever the output of
 * ClampPanelOffset — so a home the clamp would MOVE is a panel that jumps the first time it is
 * picked up. Clamping the home must therefore change nothing, on every row including the degenerate
 * ones.
 *
 * THE DEGENERATE ROWS ARE THE CLAMP'S EIGHT AGAIN, PLUS THE ONE THIS FUNCTION ADDS. A margin is a
 * distance, so a NaN margin is a distance that is not a number and a NEGATIVE margin is a distance
 * that pushes the panel off the edge it was measured from — neither is a smaller margin, and
 * neither survives FMath::Max, which discards a NaN, or FMath::Min, which replaces it. Every one of
 * those comes back as a perfectly plausible corner unless it is refused, which is the whole reason
 * they are rows.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. Five doubles in, two out.
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

	/*
	 * THE ARGUED HOME ON AN ORDINARY SCREEN, SPELLED AS THE SUBTRACTION RATHER THAN AS 1256 AND 260.
	 * The panel's right edge stands 24 px in from the viewport's, and its height is centred: the two
	 * numbers a reader has to hold are the panel and the screen, exactly as they are for the clamp's
	 * far corner above.
	 */
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
			/*
			 * A SMALLER PANEL TUCKS FURTHER RIGHT AND STAYS CENTRED, WHICH IS THE ROW THAT SAYS THIS
			 * IS A FUNCTION OF THE PANEL. A home written down as a corner passes the row above and
			 * fails this one, leaving a compact panel floating 192 px short of the edge it is
			 * supposed to be against.
			 */
			TEXT("a smaller panel, as a compact mode will ask for"),
			SmallPanel, Screen, PanelHomeMarginPx,
			SmallPanelHome, true
		},
		{
			/*
			 * NO MARGIN IS THE FAR CORNER EXACTLY, WHICH IS THE BOUNDARY THE CLAMP ALREADY CALLS
			 * INSIDE ITSELF. The panel's right edge lands on the viewport's and not a pixel past, so
			 * a home written with a > where a >= was meant would lose a pixel here and nowhere else.
			 */
			TEXT("no margin at all puts the panel flush against the right edge"),
			Panel, Screen, 0.0,
			FVector2D(PanelMaxOffsetXPx, (PanelViewportHeightPx - PanelHeightPx) * 0.5), false
		},
		{
			/*
			 * A MARGIN WIDER THAN THE ROOM LEFT OVER, WHICH IS THE INVERTED-RANGE CASE ARRIVING BY A
			 * DIFFERENT ROUTE FROM THE CLAMP'S. 1920 - 640 - 2000 is -720: a plausible-looking
			 * number that puts the panel's heading off the left of the screen. The vertical axis is
			 * an ordinary centred one on the same call, so an implementation that gave up on the
			 * whole vector when one axis was impossible is caught here too.
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
			/* A window smaller than the panel on BOTH axes, which is the one that must not go negative. */
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
			/* A negative size WIDENS the room the subtraction thinks it has: the fail-OPEN direction. */
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
			 * A NEGATIVE MARGIN IS NOT A SMALLER MARGIN. It is a distance that pushes the panel PAST
			 * the edge it was measured from — 1920 - 640 + 24 is 1304, twenty-four pixels of panel
			 * hanging off the right of the screen — and the clamp would then quietly pull it back to
			 * 1280, so the fault would be invisible rather than absent. It arrives from exactly the
			 * arithmetic a negative viewport does, and it is refused for the same reason.
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

		/*
		 * FINITE FIRST, AND SEPARATELY FROM THE VALUE, for the reason the clamp's rows give: a NaN
		 * compares false against everything including itself, so an equality alone reports the fault
		 * as an ordinary wrong number.
		 */
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

		/*
		 * THE CORNER THE PLAYER HAS TO BE ABLE TO GRAB IS ON SCREEN, SWEPT OVER EVERY ROW RATHER
		 * THAN WRITTEN PER CASE — the same invariant the clamp's rows carry, and the one a mistyped
		 * expectation would still be held to.
		 */
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

		/*
		 * AND THE CLAMP LEAVES IT ALONE. The home is written into the same field a drag writes to,
		 * and that field is only ever ClampPanelOffset's output — so a home the clamp would move is
		 * a panel that jumps the first time it is picked up, which is the join rather than either
		 * function.
		 */
		const FVector2D Clamped = ClampPanelOffset(Home, Case.PanelSizePx, Case.ViewportSizePx);

		TestEqual(
			FString::Printf(
				TEXT("%s: the home must already be a clamped offset, %s clamps to %s"),
				Case.Description, *DescribeOffset(Home), *DescribeOffset(Clamped)),
			Clamped, Home);

		/*
		 * AND ON THE SCREENS WHERE THE ARGUED HOME FITS, IT IS THE ARGUED HOME: right of the
		 * screen's own middle, and centred down it. This is the decision rather than the number —
		 * a right-anchored placement is what keeps the readout off the wall the player is pointing
		 * at, and it is the property that was lost.
		 */
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

		/* And the whole panel is on screen, which is what "tucked against the edge" has to mean. */
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
