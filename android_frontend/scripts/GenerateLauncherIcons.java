import java.awt.AlphaComposite;
import java.awt.Color;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.geom.Ellipse2D;
import java.awt.geom.RoundRectangle2D;
import java.awt.image.BufferedImage;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import javax.imageio.ImageIO;

/**
 * Generates Android density assets from Electron's transparent master icon.
 * Run from the repository root with:
 *   java android_frontend/scripts/GenerateLauncherIcons.java
 */
public final class GenerateLauncherIcons {
    private static final Color BACKGROUND = new Color(0x10, 0x15, 0x1B);

    private record Density(String qualifier, float scale) {}

    private static final Density[] DENSITIES = {
        new Density("mdpi", 1f),
        new Density("hdpi", 1.5f),
        new Density("xhdpi", 2f),
        new Density("xxhdpi", 3f),
        new Density("xxxhdpi", 4f)
    };

    public static void main(String[] args) throws IOException {
        Path root = args.length == 0 ? Paths.get("").toAbsolutePath() : Paths.get(args[0]);
        Path sourcePath = root.resolve("electron-frontend/icon_transparent.png");
        Path resources = root.resolve("android_frontend/app/src/main/res");
        BufferedImage source = ImageIO.read(sourcePath.toFile());
        if (source == null) throw new IOException("Unable to read " + sourcePath);

        for (Density density : DENSITIES) {
            Path directory = resources.resolve("mipmap-" + density.qualifier());
            Files.createDirectories(directory);

            int legacySize = Math.round(48 * density.scale());
            writeLegacy(source, directory.resolve("ic_launcher.png"), legacySize, false);
            writeLegacy(source, directory.resolve("ic_launcher_round.png"), legacySize, true);

            int adaptiveSize = Math.round(108 * density.scale());
            writeAdaptiveLayer(source, directory.resolve("ic_launcher_foreground.png"),
                adaptiveSize, false);
            writeAdaptiveLayer(source, directory.resolve("ic_launcher_monochrome.png"),
                adaptiveSize, true);
        }
    }

    private static void writeLegacy(BufferedImage source, Path output, int size, boolean round)
            throws IOException {
        BufferedImage image = new BufferedImage(size, size, BufferedImage.TYPE_INT_ARGB);
        Graphics2D graphics = graphics(image);
        graphics.setColor(BACKGROUND);
        if (round) {
            graphics.fill(new Ellipse2D.Float(0, 0, size, size));
        } else {
            float radius = size * 0.22f;
            graphics.fill(new RoundRectangle2D.Float(0, 0, size, size, radius, radius));
        }
        drawCentered(graphics, source, size, round ? 0.78f : 0.84f);
        graphics.dispose();
        ImageIO.write(image, "png", output.toFile());
    }

    private static void writeAdaptiveLayer(BufferedImage source, Path output, int size,
                                            boolean monochrome) throws IOException {
        BufferedImage image = new BufferedImage(size, size, BufferedImage.TYPE_INT_ARGB);
        Graphics2D graphics = graphics(image);
        graphics.setComposite(AlphaComposite.Src);
        drawCentered(graphics, source, size, 0.61f);
        graphics.dispose();

        if (monochrome) {
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    int alpha = image.getRGB(x, y) >>> 24;
                    image.setRGB(x, y, alpha == 0 ? 0 : (alpha << 24) | 0x00FFFFFF);
                }
            }
        }
        ImageIO.write(image, "png", output.toFile());
    }

    private static Graphics2D graphics(BufferedImage image) {
        Graphics2D graphics = image.createGraphics();
        graphics.setRenderingHint(RenderingHints.KEY_ANTIALIASING,
            RenderingHints.VALUE_ANTIALIAS_ON);
        graphics.setRenderingHint(RenderingHints.KEY_INTERPOLATION,
            RenderingHints.VALUE_INTERPOLATION_BICUBIC);
        graphics.setRenderingHint(RenderingHints.KEY_RENDERING,
            RenderingHints.VALUE_RENDER_QUALITY);
        return graphics;
    }

    private static void drawCentered(Graphics2D graphics, BufferedImage source, int canvas,
                                     float widthFraction) {
        int width = Math.round(canvas * widthFraction);
        int height = Math.round(width * source.getHeight() / (float) source.getWidth());
        int x = (canvas - width) / 2;
        int y = (canvas - height) / 2;
        graphics.drawImage(source, x, y, width, height, null);
    }
}

