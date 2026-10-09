// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// The Control Panel's design tokens as numbers: the spacing scale, the
   /// corner radii, the shell's fixed dimensions, and the colours that are not
   /// the three status colours (<see cref="StatusPalette"/> keeps those, with the
   /// test that holds them apart for colour-blind eyes).
   ///
   /// This is the one place the values live. Views/Scaffold/Tokens.xaml declares
   /// the same values as resources for XAML, and <see cref="ThemeTokens"/> turns
   /// the colours into brushes on every theme change; the doc that names every
   /// token is docs/ControlPanelDesign.md. Being plain numbers, the values can be
   /// held by a test without WPF - that the scale is monotonic and on a
   /// four-pixel grid, that High Contrast derives every colour from the system
   /// palette and invents none, that the information colour stays apart from the
   /// three status colours under the two common dichromacies.
   /// </summary>
   public static class DesignTokens
   {
      /// <summary>
      /// The spacing scale, in device-independent pixels. Every margin, padding
      /// and gap in the scaffold is one of these six; a page that needs a seventh
      /// is a page whose layout has gone wrong.
      /// </summary>
      public static class Space
      {
         public const double Xs = 4;
         public const double Sm = 8;
         public const double Md = 12;
         public const double Lg = 16;
         public const double Xl = 24;
         public const double Xxl = 32;

         /// <summary>The whole scale, ascending. Only used by the tests today.</summary>
         public static IReadOnlyList<double> All { get; } = new[] { Xs, Sm, Md, Lg, Xl, Xxl };
      }

      /// <summary>
      /// Corner radii. Controls and rows take the small one, cards and dialogs
      /// the large one. The status pill is a filled box with the control's
      /// radius: it was a capsule outlined in the level's colour, which read as
      /// a stray line drawn round the word rather than as a box the word is in.
      /// </summary>
      public static class Radius
      {
         public const double Control = 4;
         public const double Card = 8;
         public const double Pill = 4;
      }

      /// <summary>The shell's fixed dimensions.</summary>
      public static class Shell
      {
         /// <summary>The command bar under the title bar.</summary>
         public const double CommandBarHeight = 48;

         /// <summary>The sidebar when it is collapsed to icons: one group glyph per row.</summary>
         public const double RailWidth = 48;

         /// <summary>Below this window width the sidebar collapses to the rail on its own; above it the user's choice stands.</summary>
         public const double CollapseSidebarBelow = 1000;

         /// <summary>The sidebar's expanded width limits - sized by the longest group name, not by taste (see MainWindow.xaml).</summary>
         public const double SidebarMinWidth = 200;
         public const double SidebarMaxWidth = 320;

         /// <summary>One navigation row, expanded or in the rail.</summary>
         public const double NavRowHeight = 34;
      }

      /// <summary>
      /// Dialog sizing. A dialog is sized to its content within these bounds
      /// instead of to a number typed into each one; the height is capped at a
      /// fraction of the screen's work area and the body scrolls past it.
      /// </summary>
      public static class Dialog
      {
         public const double MinWidth = 360;
         public const double DefaultWidth = 520;
         public const double MaxWidth = 900;
         public const double MaxHeightFraction = 0.9;
      }

      /// <summary>
      /// The widest a form is laid out, whatever the window does. A settings
      /// page in a maximised window would otherwise run a caption and its
      /// editor to opposite ends of a 2,560-pixel screen, which is unreadable
      /// and which both settings waves worked around with a private constant of
      /// the same value before it was a token.
      /// </summary>
      public const double FormWidth = 760;

      /// <summary>
      /// The tint behind an inline notice: the notice's status colour at this
      /// opacity. Zero under High Contrast, where a tinted surface is a colour
      /// the user asked Windows not to show them and the border carries the
      /// notice instead.
      /// </summary>
      public const double NoticeTintOpacity = 0.12;

      public static double NoticeTintOpacityFor(ChartTheme theme)
         => theme == ChartTheme.HighContrast ? 0 : NoticeTintOpacity;

      /// <summary>
      /// A status pill's box for one level: the fill, and the colour of the
      /// word and the mark drawn on it, as 0xAARRGGBB. The text colour is not
      /// the level's status colour but a deeper (light theme) or brighter
      /// (dark theme) shade of it, because the status colours are measured
      /// against the page and lose that contrast on a tinted fill: the light
      /// success green on the old grey pill measured 4.2:1. Every pair here
      /// clears 4.5:1 with room to spare, and a test holds them to it.
      /// </summary>
      public readonly struct StatusFillArgb
      {
         public StatusFillArgb(uint fill, uint text)
         {
            Fill = fill;
            Text = text;
         }

         public uint Fill { get; }

         /// <summary>The word and the mark on the fill.</summary>
         public uint Text { get; }
      }

      /// <summary>
      /// The pill's fill and text for a level and a theme. Normal is the
      /// neutral grey. Under High Contrast the fill is the window colour and
      /// the text the level's own system colour - a tinted fill is a colour the
      /// user asked Windows not to show them - and the box keeps its outline
      /// instead (<see cref="StatusFillBorderFor"/>).
      /// </summary>
      public static StatusFillArgb StatusFill(StatusLevel level, ChartTheme theme, ChartSystemColors system)
      {
         if (theme == ChartTheme.HighContrast)
         {
            StatusArgb status = Status(theme, system);
            uint text = level switch
            {
               StatusLevel.Good => status.Success,
               StatusLevel.Information => status.Info,
               StatusLevel.Warning => status.Warning,
               StatusLevel.Critical => status.Danger,
               _ => status.Neutral
            };
            return new StatusFillArgb(system.Window, text);
         }

         bool light = theme == ChartTheme.Light;
         return level switch
         {
            StatusLevel.Good => light ? new StatusFillArgb(0xFFCCF0D6, 0xFF116329) : new StatusFillArgb(0xFF1C4128, 0xFF56D364),
            StatusLevel.Information => light ? new StatusFillArgb(0xFFEBE0FF, 0xFF5A32A3) : new StatusFillArgb(0xFF3A2A63, 0xFFC8A2FF),
            StatusLevel.Warning => light ? new StatusFillArgb(0xFFFBE7AE, 0xFF7D4E00) : new StatusFillArgb(0xFF45350F, 0xFFE3B341),
            StatusLevel.Critical => light ? new StatusFillArgb(0xFFFFDAD6, 0xFFA40E26) : new StatusFillArgb(0xFF522024, 0xFFFF9492),
            _ => light ? new StatusFillArgb(0xFFDFE4E9, 0xFF424A53) : new StatusFillArgb(0xFF3A4048, 0xFFD1D9E0)
         };
      }

      /// <summary>
      /// The light theme's tertiary text, which replaces Wpf.Ui's. Wpf.Ui's
      /// light tertiary is black at 45% (#72000000), which composites to #8B8B8B
      /// on the page and measures 3.3:1 against it - and the Control Panel
      /// sets real sentences in it: every field's hint and "Applies on save"
      /// line, the Related links' caption, the search box's shortcut. Black at
      /// 58% composites to #696969 on the page (5.3:1) and #656565 on the
      /// darkest light surface, a hovered row (5.1:1), and stays a step lighter
      /// than the secondary text (62%). The dark theme's tertiary (#87FFFFFF,
      /// 6.4:1 on the page) and High Contrast's are left as the theme has them.
      /// </summary>
      public const uint LightTertiaryText = 0x94000000;

      /// <summary>The tertiary text colour this theme needs in place of Wpf.Ui's, or null to keep the theme's own.</summary>
      public static uint? TertiaryTextFor(ChartTheme theme)
         => theme == ChartTheme.Light ? LightTertiaryText : null;

      /// <summary>
      /// Where the keyboard is: the colour of the ring round a text field that
      /// has the focus, and of the glow outside it (<c>FocusGlow</c>), as
      /// 0xAARRGGBB. The glow is the ring's colour, translucent.
      /// </summary>
      public readonly struct FocusArgb
      {
         public FocusArgb(uint ring, uint glow)
         {
            Ring = ring;
            Glow = glow;
         }

         public uint Ring { get; }

         /// <summary>The glow at its strongest, the pixel beside the ring; it fades from there (<see cref="FocusGlowOpacityAt"/>).</summary>
         public uint Glow { get; }
      }

      // The violet the Control Deck and the webmail declare as --focus and
      // --focus-glow, value for value, so that the three interfaces have one
      // focus colour: darker on the light theme, lighter on the dark. Each ring
      // clears 3:1 (WCAG 2.2, SC 1.4.11) against every surface a field sits on
      // and against the glow at its strongest beside it - DesignTokensTests
      // holds both, and the pages' copies to these values.
      private const uint LightFocus = 0xFF7542E5;
      private const uint LightFocusGlow = 0x407542E5;
      private const uint DarkFocus = 0xFFB89CFF;
      private const uint DarkFocusGlow = 0x47B89CFF;

      /// <summary>
      /// The focus colours for a theme. High Contrast invents none: the ring is
      /// the system's highlight and there is no glow - and <see cref="FocusGlowFor"/>
      /// says not to draw either, so the theme's own focus visuals stand.
      /// </summary>
      public static FocusArgb Focus(ChartTheme theme, ChartSystemColors system)
      {
         switch (theme)
         {
            case ChartTheme.HighContrast:
               return new FocusArgb(system.Highlight, 0);

            case ChartTheme.Light:
               return new FocusArgb(LightFocus, LightFocusGlow);

            default:
               return new FocusArgb(DarkFocus, DarkFocusGlow);
         }
      }

      /// <summary>Whether a focused field is drawn with the ring and glow in this theme: not under High Contrast, whose colours are the user's.</summary>
      public static bool FocusGlowFor(ChartTheme theme) => theme != ChartTheme.HighContrast;

      /// <summary>
      /// The ring's width, drawn inside the field's edge over its own border, so
      /// that it keeps the field's size and corner radius and moves nothing.
      /// </summary>
      public const double FocusRingThickness = 2;

      /// <summary>How far the glow reaches outside the field, in one-pixel steps.</summary>
      public const int FocusGlowWidth = 6;

      /// <summary>
      /// The glow's opacity at a step outside the ring, as a fraction of
      /// <see cref="FocusArgb.Glow"/>'s own: all of it beside the ring, falling
      /// away with the square of the distance, nothing past the last step.
      /// </summary>
      public static double FocusGlowOpacityAt(int step)
      {
         if (step < 0 || step >= FocusGlowWidth)
            return 0;
         double left = (double)(FocusGlowWidth - step) / FocusGlowWidth;
         return left * left;
      }

      /// <summary>
      /// Whether a field with this border has a box for the ring to go round: a
      /// border on its left, top and right. The bottom need not be the border's:
      /// Wpf.Ui's date picker draws its bottom edge as the theme's accent line
      /// (a border of 1,1,1,0) and is a box all the same. A field drawn as an
      /// underline (the search palette's) shows the focus colour in its
      /// underline instead, through the theme's own accent line; a field with no
      /// border at all is a value shown as text, and is not given a box it does
      /// not have.
      /// </summary>
      public static bool DrawsFocusRing(double left, double top, double right, double bottom)
         => left > 0 && top > 0 && right > 0;

      /// <summary>
      /// How far the glow may be drawn, in the field's own coordinates (its box
      /// is 0,0 to width,height), given the part of it in view - what the scroll
      /// viewers and clipping panels it is in leave of it: past an edge that is
      /// in view the glow's whole width, so that a field as wide as a dialog's
      /// scrolling body keeps its glow at the sides; at an edge a viewer cuts
      /// through, the cut, so that a field scrolled half out of its page draws
      /// nothing over what is beyond the viewer.
      /// </summary>
      public static (double Left, double Top, double Right, double Bottom) FocusGlowBounds(
         double inViewLeft, double inViewTop, double inViewRight, double inViewBottom, double width, double height)
      {
         const double edge = 0.5;
         return (inViewLeft <= edge ? Math.Min(inViewLeft, -FocusGlowWidth) : inViewLeft,
                 inViewTop <= edge ? Math.Min(inViewTop, -FocusGlowWidth) : inViewTop,
                 inViewRight >= width - edge ? Math.Max(inViewRight, width + FocusGlowWidth) : inViewRight,
                 inViewBottom >= height - edge ? Math.Max(inViewBottom, height + FocusGlowWidth) : inViewBottom);
      }

      /// <summary>
      /// The pill's outline, in pixels: none in the light and dark themes,
      /// where the fill is the box, and one under High Contrast, where the
      /// fill is the window colour and only an outline can draw the box.
      /// </summary>
      public static double StatusFillBorderFor(ChartTheme theme)
         => theme == ChartTheme.HighContrast ? 1 : 0;

      /// <summary>
      /// A selected row's look, one language for every grid and list: the fill
      /// behind the row and the outline round it, as 0xAARRGGBB. The fill is
      /// the brand colour at the notice tint's opacity over whatever the row
      /// sits on, so near the card's own that the row's text stays what it was
      /// - the theme's primary and secondary text clear 4.5:1 on it, where the
      /// stock accent band drew the palette's at 1.9 to 3.5:1 - and the
      /// sidebar's brand pill at the row's left edge says "selected" apart from
      /// the hover grey, which a selected grid row was the same as (1.08:1
      /// against its neighbours); the sidebar and the rail themselves keep the
      /// subtle grey behind their pill. Under High Contrast the fill is nothing,
      /// as the notice tint is - a tinted surface is a colour the user asked
      /// Windows not to show them - and the outline is the highlight, as the
      /// pill is; in the other themes the outline is nothing.
      /// </summary>
      public readonly struct SelectionArgb
      {
         public SelectionArgb(uint fill, uint outline)
         {
            Fill = fill;
            Outline = outline;
         }

         /// <summary>Behind the selected row; transparent under High Contrast.</summary>
         public uint Fill { get; }

         /// <summary>The one-pixel outline round it; transparent but under High Contrast.</summary>
         public uint Outline { get; }
      }

      /// <summary>The selected row's fill and outline for a theme (<see cref="SelectionArgb"/>).</summary>
      public static SelectionArgb Selection(ChartTheme theme, ChartSystemColors system)
      {
         if (theme == ChartTheme.HighContrast)
            return new SelectionArgb(0x00000000, system.Highlight);

         uint brand = Status(theme, system).Brand & 0x00FFFFFF;
         uint alpha = (uint)Math.Round(NoticeTintOpacity * 255);
         return new SelectionArgb((alpha << 24) | brand, 0x00000000);
      }

      /// <summary>The status palette for one theme, as 0xAARRGGBB.</summary>
      public readonly struct StatusArgb
      {
         public StatusArgb(uint brand, uint success, uint warning, uint danger, uint info, uint neutral)
         {
            Brand = brand;
            Success = success;
            Warning = warning;
            Danger = danger;
            Info = info;
            Neutral = neutral;
         }

         public uint Brand { get; }
         public uint Success { get; }
         public uint Warning { get; }
         public uint Danger { get; }
         public uint Info { get; }

         /// <summary>A status that says nothing - "not connected", a count of zero. Secondary text, never a colour of its own.</summary>
         public uint Neutral { get; }
      }

      /// <summary>The surfaces for one theme, as 0xAARRGGBB. The translucent ones sit on Mica and on a solid page alike.</summary>
      public readonly struct SurfaceArgb
      {
         public SurfaceArgb(uint cardBackground, uint cardBorder, uint cardHover, uint divider)
         {
            CardBackground = cardBackground;
            CardBorder = cardBorder;
            CardHover = cardHover;
            Divider = divider;
         }

         public uint CardBackground { get; }
         public uint CardBorder { get; }

         /// <summary>A card that can be pointed at - a tile, a rail item - under the pointer.</summary>
         public uint CardHover { get; }

         /// <summary>The hairline between a card's content and its footer, and between the shell's regions.</summary>
         public uint Divider { get; }
      }

      // The light values are darker and saturated so that each clears 4.5:1 on a
      // white surface; the dark values are lighter and brighter for dark surfaces.
      // The three status colours come from StatusPalette; these are the rest.
      private const uint LightBrand = 0xFF2F6FE0;
      private const uint LightInfo = 0xFF6639BA;
      private const uint LightNeutral = 0xFF57606A;
      private const uint DarkBrand = 0xFF4C8DFF;
      private const uint DarkInfo = 0xFFA371F7;
      private const uint DarkNeutral = 0xFF9DA7B0;

      /// <summary>
      /// The status palette for a theme. High Contrast takes every colour from
      /// the system palette and invents none: the brand and information colours
      /// are the highlight, the three status colours are the window text (their
      /// shape and word carry the difference - <see cref="StatusSemantics"/>), and
      /// neutral is the disabled-text grey.
      /// </summary>
      public static StatusArgb Status(ChartTheme theme, ChartSystemColors system)
      {
         switch (theme)
         {
            case ChartTheme.HighContrast:
               return new StatusArgb(system.Highlight, system.WindowText, system.WindowText, system.WindowText,
                  system.Highlight, system.GrayText);

            case ChartTheme.Light:
               {
                  (uint success, uint warning, uint danger) = StatusPalette.Argb(light: true);
                  return new StatusArgb(LightBrand, success, warning, danger, LightInfo, LightNeutral);
               }

            default:
               {
                  (uint success, uint warning, uint danger) = StatusPalette.Argb(light: false);
                  return new StatusArgb(DarkBrand, success, warning, danger, DarkInfo, DarkNeutral);
               }
         }
      }

      /// <summary>
      /// The surfaces for a theme. The light and dark values are the ones the
      /// Fluent control-fill and stroke layers use, so a card matches the
      /// controls on it; High Contrast paints the window colour with a window-text
      /// outline, which is the only card that theme allows.
      /// </summary>
      public static SurfaceArgb Surfaces(ChartTheme theme, ChartSystemColors system)
      {
         switch (theme)
         {
            case ChartTheme.HighContrast:
               return new SurfaceArgb(system.Window, system.WindowText, system.Window, system.WindowText);

            case ChartTheme.Light:
               return new SurfaceArgb(0xB3FFFFFF, 0x0F000000, 0x80F9F9F9, 0x0F000000);

            default:
               return new SurfaceArgb(0x0FFFFFFF, 0x14FFFFFF, 0x15FFFFFF, 0x14FFFFFF);
         }
      }

      /// <summary>
      /// The live log's colour for each kind of line, as 0xAARRGGBB: a line that
      /// is none of the others, SMTP, IMAP, POP3, the server's own application
      /// lines, and errors. Not design tokens - the log viewer's own palette - but
      /// held here with them so that a test can measure them: every one clears
      /// 4.5:1 on the page, on the card the log sits on, on a line under the
      /// pointer (the theme's subtle grey) and on a selected line (the
      /// selection fill), which are what the log's lines draw, each being the
      /// application's list row (LogsView.xaml). The dark values were measured
      /// against #1B1B1B only, and on the dark card (#2D2D2D) the error and
      /// IMAP lines were 4.1:1 and the application's 4.3:1; the light theme's
      /// application and POP3 lines were 4.3 and 4.5:1 on a hovered line, and
      /// its SMTP line, #1A7F37, under 4.5:1 on a selected one (4.0:1 on the
      /// system's selection the log drew until 9 October 2026, 4.3:1 on the
      /// application's). Under High Contrast each is a system colour.
      /// </summary>
      public readonly struct LogArgb
      {
         public LogArgb(uint other, uint smtp, uint imap, uint pop3, uint application, uint error)
         {
            Other = other;
            Smtp = smtp;
            Imap = imap;
            Pop3 = pop3;
            Application = application;
            Error = error;
         }

         public uint Other { get; }
         public uint Smtp { get; }
         public uint Imap { get; }
         public uint Pop3 { get; }
         public uint Application { get; }
         public uint Error { get; }
      }

      /// <summary>The live log's colours for a theme (<see cref="LogArgb"/>).</summary>
      public static LogArgb Log(ChartTheme theme, ChartSystemColors system)
      {
         switch (theme)
         {
            case ChartTheme.HighContrast:
               return new LogArgb(system.GrayText, system.WindowText, system.WindowText, system.WindowText, system.Highlight, system.HotTrack);

            case ChartTheme.Light:
               return new LogArgb(0xFF57606A, 0xFF116329, 0xFF6639BA, 0xFF8A5C00, 0xFF1F62D0, 0xFFCF222E);

            default:
               return new LogArgb(0xFF9DA7B0, 0xFF3FB950, 0xFFBC8CFF, 0xFFD29922, 0xFF58A6FF, 0xFFFF7B72);
         }
      }

      /// <summary>
      /// A filled destructive button - a confirmation's destructive choice,
      /// Start restore, every <c>Appearance="Danger"</c> - as 0xAARRGGBB: the fill
      /// at rest, under the pointer and pressed, and the words on it. WPF-UI drew
      /// it in its own #F44336 in both themes with the theme's primary text on it,
      /// which is white in the dark theme and measured 3.7:1; these are the
      /// danger colour of the palette filled, white words on the light theme's
      /// and near-black on the dark theme's, each pair 4.5:1 or more in every
      /// state. Under High Contrast the button is the window text with the window
      /// colour on it, both the user's, and is the same in every state.
      /// </summary>
      public readonly struct DangerSolidArgb
      {
         public DangerSolidArgb(uint fill, uint hover, uint pressed, uint text)
         {
            Fill = fill;
            Hover = hover;
            Pressed = pressed;
            Text = text;
         }

         public uint Fill { get; }
         public uint Hover { get; }
         public uint Pressed { get; }

         /// <summary>The words on the fill, in every state.</summary>
         public uint Text { get; }
      }

      /// <summary>The filled destructive button for a theme (<see cref="DangerSolidArgb"/>).</summary>
      public static DangerSolidArgb DangerSolid(ChartTheme theme, ChartSystemColors system)
      {
         switch (theme)
         {
            case ChartTheme.HighContrast:
               return new DangerSolidArgb(system.WindowText, system.WindowText, system.WindowText, system.Window);

            case ChartTheme.Light:
               return new DangerSolidArgb(StatusPalette.LightDanger, 0xFFB91C28, 0xFFA40E26, 0xFFFFFFFF);

            default:
               return new DangerSolidArgb(StatusPalette.DarkDanger, 0xFFFA6B64, 0xFFE5534B, 0xFF1B1B1B);
         }
      }

      /// <summary>
      /// A link's colour at rest and under the pointer, as 0xAARRGGBB: the About
      /// page's, the help pane's document list and every link inside a help
      /// document. A link took WPF's own colour, the system's #0066CC, which is
      /// 2.5:1 on the dark card, and turned red under the pointer, the danger
      /// colour. These clear 4.5:1 on the page, a card, a dialog and a hovered or
      /// selected row in their theme, under the pointer as well. Under High
      /// Contrast a link is the system's own link colour, HotTrack.
      /// </summary>
      public readonly struct LinkArgb
      {
         public LinkArgb(uint rest, uint hover)
         {
            Rest = rest;
            Hover = hover;
         }

         public uint Rest { get; }
         public uint Hover { get; }
      }

      /// <summary>The link colours for a theme (<see cref="LinkArgb"/>).</summary>
      public static LinkArgb Link(ChartTheme theme, ChartSystemColors system)
      {
         switch (theme)
         {
            case ChartTheme.HighContrast:
               return new LinkArgb(system.HotTrack, system.HotTrack);

            case ChartTheme.Light:
               return new LinkArgb(0xFF0B5CB5, 0xFF084A94);

            default:
               return new LinkArgb(0xFF6CB6FF, 0xFF9CCFFF);
         }
      }

      /// <summary>
      /// A text field's border at rest, as 0xAARRGGBB: its bottom edge, and the
      /// rest of its outline. Fluent's own drew a field's outline at 1.1:1 and
      /// its bottom edge at 1.5 to 1.7:1, so that an empty field was a faint band
      /// on its card; the bottom edge is now the theme's strong stroke
      /// (ControlStrongStrokeColorDefault), 3:1 or more against the card, the
      /// page, a dialog and the field's own fill, which is what a reader finds a
      /// field by, and the outline stays the theme's subtle one
      /// (ControlStrokeColorDefault). The focused field keeps its accent line and
      /// FocusGlow's ring. Null under High Contrast, whose own border stands.
      /// </summary>
      public readonly struct TextFieldStrokeArgb
      {
         public TextFieldStrokeArgb(uint edge, uint outline)
         {
            Edge = edge;
            Outline = outline;
         }

         /// <summary>The bottom edge, one pixel, translucent over what the field sits on.</summary>
         public uint Edge { get; }

         /// <summary>The top and sides.</summary>
         public uint Outline { get; }
      }

      /// <summary>The text field's border at rest for a theme, or null to keep the theme's own (<see cref="TextFieldStrokeArgb"/>).</summary>
      public static TextFieldStrokeArgb? TextFieldStroke(ChartTheme theme)
      {
         switch (theme)
         {
            case ChartTheme.HighContrast:
               return null;

            case ChartTheme.Light:
               return new TextFieldStrokeArgb(0x72000000, 0x0F000000);

            default:
               return new TextFieldStrokeArgb(0x8BFFFFFF, 0x12FFFFFF);
         }
      }
   }
}
