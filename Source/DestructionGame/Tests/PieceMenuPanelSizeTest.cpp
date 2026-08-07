// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many
 * files into one. See CURRENT_STATE.md and Tests/ConnectionLoadTest.cpp, which is where that rule
 * was paid for; the `using namespace` lives inside RunTest for the same reason.
 */
namespace PieceMenuPanelSizeTestSupport
{
	/**
	 * THE PANEL AS IT IS DRAWN TODAY, TRANSCRIBED RATHER THAN IMPORTED, AND IT IS 640 x 560 px.
	 *
	 * PieceMenuPanelWidthPx and PieceMenuPanelHeightPx are file-static in
	 * DestructionGamePlayerController.cpp and a test may not reach them — which is the right way
	 * round, and is about to stop mattering: the whole point of this test is that the size becomes
	 * the PRESENTER's answer, so that the mode can change it and something can read what it said.
	 *
	 * NEITHER NUMBER IS PICKED, AND THE WIDTH IS THE ONE WITH A DERIVATION. 640 px is a measured
	 * floor plus a stated clearance: World.Menu.TheReadoutFitsInsideThePanel lays the readout out
	 * over a ragged wall whose corbel sits on one off-centre patch — the only wall shape that bends
	 * at all, and so the only one whose joint line grows the bending clause — and puts the floor at
	 * 617.5 px. 640 clears it by 22 px, about four characters at the readout's font, which is the
	 * room a course number in the hundreds still wants in the game's own 1,220-brick wall. The
	 * height is a fit rather than a measurement: 560 px is about half a 1080 viewport, so the list,
	 * the readout and the action rows are all on screen at once.
	 *
	 * PINNING THEM HERE IS WHAT KEEPS *Full* HONEST WHILE COMPACT IS ADDED. A mode-dependent size
	 * whose Full arm quietly drifted would take the measured floor with it, and the wall that can
	 * see that floor is expensive to build — so the cheap claim lives here and the expensive one
	 * stays where the pixels are.
	 */
	constexpr double FullPanelWidthPx = 640.0;
	constexpr double FullPanelHeightPx = 560.0;

	/**
	 * HOW MUCH OF THE PANEL COMPACT HAS TO GIVE BACK, AND IT IS THE MODEL'S OWN CLAIM RATHER THAN
	 * A NUMBER CHOSEN HERE.
	 *
	 * EPieceMenuDetail::Compact says of itself, in Core/PieceMenu.h, that dropping the joint table
	 * "is the cut that buys back a third of the screen". A third back is two thirds left, and that
	 * is the whole of this constant — read as a fraction of the PANEL, which is the conservative
	 * reading of the sentence and the only one this function is in a position to promise.
	 *
	 * IT IS AREA RATHER THAN EITHER AXIS BECAUSE "STRICTLY SMALLER" IS SATISFIED BY ONE PIXEL. A
	 * compact panel 639 x 559 px is smaller in both axes, passes every ordering claim below, and
	 * answers the player's complaint — "it takes up so much of the screen" — with nothing at all.
	 * The per-axis claims say WHICH WAY it must shrink; this one says it has to be worth doing.
	 */
	constexpr double CompactPanelAreaBudget = 2.0 / 3.0;

	const TCHAR* NameOfDetail(EPieceMenuDetail Detail)
	{
		switch (Detail)
		{
		case EPieceMenuDetail::Full:    return TEXT("Full");
		case EPieceMenuDetail::Compact: return TEXT("Compact");
		}

		return TEXT("<not a detail mode>");
	}

	FString DescribeSize(const FVector2D& SizePx)
	{
		return FString::Printf(TEXT("%.2f x %.2f px"), SizePx.X, SizePx.Y);
	}

	bool SizeIsFinite(const FVector2D& SizePx)
	{
		return FMath::IsFinite(SizePx.X) && FMath::IsFinite(SizePx.Y);
	}

	/** One row: a mode the panel can be asked for, and what to call it in a failure. */
	struct FPanelSizeCase
	{
		const TCHAR* Description;
		EPieceMenuDetail Detail;
	};
}

/**
 * A COMPACT PANEL IS A SMALLER PANEL — STRICTLY NARROWER *AND* STRICTLY SHORTER THAN THE FULL ONE
 * — AND HOW BIG EITHER OF THEM IS, IS THE PRESENTER'S ANSWER RATHER THAN A CONSTANT BESIDE THE
 * SLATE THAT DRAWS IT.
 *
 * THE COMPLAINT THIS IS THE SECOND HALF OF, VERBATIM: "I can't move the menu if it is in my way
 * and it takes up so much of the screen." The first half is answered — ClampPanelOffset is in and
 * the title strip drags. The second is not: EPieceMenuDetail::Compact drops the joint table and the
 * headroom scale, and then draws the lines that are left into a rectangle overridden to exactly
 * 640 x 560 px whatever the mode, with the readout in a slot that FILLS whatever it is given. So a
 * compact panel is a full-sized panel with fewer words in it, and the player gets back no screen at
 * all. Making the size a function of the mode is the behaviour; making it a function LIVING HERE is
 * what makes the behaviour assertable.
 *
 * THE ASSERTIONS ARE ORDERINGS AND A BUDGET, NOT A COMPACT SIZE. This test deliberately does not
 * say what the compact panel measures, because that figure has to be DERIVED the way the full one
 * was — from the longest line the mode still shows, which is an entry row rather than a joint row,
 * and which only a laid-out panel over a real wall can measure. That measurement is
 * World.Menu.TheReadoutFitsInsideThePanel's job and it now sweeps both modes. What is pinned here
 * is everything that measurement cannot say: that the answer exists, that it is finite and
 * positive, that Full has not drifted off its own measured floor, that Compact is smaller on BOTH
 * axes rather than one, and that it is smaller by enough to be worth a player's while.
 *
 * FULL IS PINNED EXACTLY AND COMPACT IS BOUNDED, WHICH IS THE ASYMMETRY THE DERIVATION IMPLIES.
 * 640 x 560 is a measured floor plus a stated clearance and a retune of it is a decision somebody
 * should have to make on purpose; the compact figure is a derivation this test cannot perform, so
 * pinning one here would be inventing the number the task exists to avoid inventing.
 *
 * WHAT THIS CANNOT REACH, SAID PLAINLY: whether the WIDGET honours the answer. An SBox override
 * sets a desired size and a filling slot arranges its child at whatever width it was handed — this
 * project has already shipped a 540 px canvas sitting over 96 px bars on exactly that mistake. The
 * join is measured in World.Menu.TheReadoutFitsInsideThePanel, which arranges the real panel in
 * both modes and compares what moved against what this function said.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. One enumerator in, two doubles out.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPanelSizeTest,
	"DestructionGame.Presenter.PieceMenuPanelSize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPanelSizeTest::RunTest(const FString& Parameters)
{
	using namespace PieceMenuPanelSizeTestSupport;

	const FVector2D Full = PieceMenuPanelSizePx(EPieceMenuDetail::Full);
	const FVector2D Compact = PieceMenuPanelSizePx(EPieceMenuDetail::Compact);

	/*
	 * AN ENUMERATOR NOBODY DECLARED, WHICH IS THE FAIL-CLOSED ROW. EPieceMenuDetail is a uint8 and
	 * a cast is all it takes to produce one; the direction that matters is which arm a value this
	 * function does not know falls into. Compact would SUPPRESS numbers somebody asked for, which
	 * is the failure the enum's own comment names as the reason Full is enumerator zero.
	 */
	const EPieceMenuDetail UnknownDetail = static_cast<EPieceMenuDetail>(200);

	const TArray<FPanelSizeCase> Cases = {
		{ TEXT("the full panel"), EPieceMenuDetail::Full },
		{ TEXT("the compact panel"), EPieceMenuDetail::Compact },
		{ TEXT("a detail mode this build has never heard of"), UnknownDetail },
	};

	for (const FPanelSizeCase& Case : Cases)
	{
		const FVector2D SizePx = PieceMenuPanelSizePx(Case.Detail);

		/*
		 * FINITE FIRST AND SEPARATELY FROM THE VALUE. A NaN compares false against everything
		 * including itself, so the positivity claim below would report "it is nan x nan px" as an
		 * ordinary too-small panel rather than as the fault it is.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): the panel's size must be a number, it is %s"),
				Case.Description, NameOfDetail(Case.Detail), *DescribeSize(SizePx)),
			SizeIsFinite(SizePx));

		/*
		 * AND A PANEL OF NO SIZE IS NOT A SMALL PANEL. Zero draws no heading, and the heading is
		 * the strip the whole drag affordance hangs off — a panel that cannot be grabbed is the
		 * state ClampPanelOffset exists to refuse, arrived at from the other direction.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): the panel must have a real size on both axes, it is %s"),
				Case.Description, NameOfDetail(Case.Detail), *DescribeSize(SizePx)),
			SizePx.X > 0.0 && SizePx.Y > 0.0);

		/*
		 * NO MODE IS BIGGER THAN Full ON EITHER AXIS. Full is the mode that withholds nothing, so
		 * it is the mode that needs the most room; a mode asking for more than it is asking for
		 * more room to show less.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): no mode may be larger than the full panel (%s), it asks for %s"),
				Case.Description, NameOfDetail(Case.Detail), *DescribeSize(Full), *DescribeSize(SizePx)),
			SizePx.X <= Full.X && SizePx.Y <= Full.Y);

		/*
		 * AND THE ANSWER IS THE SAME ANSWER TWICE. It is read while a widget tree is built and read
		 * again while the panel is placed; a size that moved between the two calls would be a panel
		 * anchored to a corner it is not the size of.
		 */
		TestEqual(
			FString::Printf(
				TEXT("%s (%s): asking twice must give one answer, %s became %s"),
				Case.Description, NameOfDetail(Case.Detail),
				*DescribeSize(SizePx), *DescribeSize(PieceMenuPanelSizePx(Case.Detail))),
			PieceMenuPanelSizePx(Case.Detail), SizePx);
	}

	/*
	 * THE FULL PANEL IS THE SIZE THE GAME ALREADY DRAWS, TO THE PIXEL. This is the characterisation
	 * half: the behaviour being added is COMPACT, and Full moving with it would silently retune a
	 * width that was measured against the longest sentence this readout can compose.
	 */
	TestEqual(
		FString::Printf(
			TEXT("the full panel must stay the measured 640 x 560 px, it is %s"), *DescribeSize(Full)),
		Full, FVector2D(FullPanelWidthPx, FullPanelHeightPx));

	/* And the mode nobody declared answers as Full does, rather than hiding half the readout. */
	TestEqual(
		FString::Printf(
			TEXT("a detail mode this build has never heard of must fall back to the full size (%s), it answers %s"),
			*DescribeSize(Full), *DescribeSize(PieceMenuPanelSizePx(UnknownDetail))),
		PieceMenuPanelSizePx(UnknownDetail), Full);

	/*
	 * THE TWO CLAIMS THE WHOLE FILE IS FOR, ONE PER AXIS AND SEPARATELY, BECAUSE THE TWO FAILURES
	 * LOOK DIFFERENT ON SCREEN. A panel that only narrowed leaves a full-height ribbon down the
	 * side of the viewport; one that only shortened leaves a full-width band across it. Either is
	 * a mode that dropped the joint table and kept the room the table was taking.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("the compact panel must be strictly NARROWER than the full one: %.2f px against %.2f px, giving back %.2f px"),
			Compact.X, Full.X, Full.X - Compact.X),
		Compact.X < Full.X);

	TestTrue(
		*FString::Printf(
			TEXT("the compact panel must be strictly SHORTER than the full one: %.2f px against %.2f px, giving back %.2f px"),
			Compact.Y, Full.Y, Full.Y - Compact.Y),
		Compact.Y < Full.Y);

	/*
	 * AND IT MUST GIVE BACK ENOUGH TO BE WORTH ASKING FOR. See CompactPanelAreaBudget: the model's
	 * own description of this mode is that it buys back a third, and "strictly smaller" on its own
	 * is satisfied by a pixel. Reported as a percentage either way, because the number a retune
	 * needs is how much room is left rather than whether it passed.
	 */
	const double FullAreaSqPx = Full.X * Full.Y;
	const double CompactAreaSqPx = Compact.X * Compact.Y;

	AddInfo(FString::Printf(
		TEXT("full %s = %.0f sq px; compact %s = %.0f sq px, which is %.1f %% of it"),
		*DescribeSize(Full), FullAreaSqPx, *DescribeSize(Compact), CompactAreaSqPx,
		FullAreaSqPx > 0.0 ? 100.0 * CompactAreaSqPx / FullAreaSqPx : 0.0));

	TestTrue(
		*FString::Printf(
			TEXT("the compact panel must buy back a third of the panel's area: %.0f sq px against a budget of %.0f sq px (%.1f %% of the full panel's %.0f sq px)"),
			CompactAreaSqPx, CompactPanelAreaBudget * FullAreaSqPx,
			FullAreaSqPx > 0.0 ? 100.0 * CompactAreaSqPx / FullAreaSqPx : 0.0, FullAreaSqPx),
		CompactAreaSqPx <= CompactPanelAreaBudget * FullAreaSqPx);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
