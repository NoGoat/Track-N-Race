document.addEventListener('DOMContentLoaded', () => {
    // Mobile Navigation Toggle
    const mobileToggleBtn = document.getElementById('mobileToggleBtn');
    const navMenu = document.querySelector('.nav-menu');

    if (mobileToggleBtn && navMenu) {
        mobileToggleBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            const isOpen = navMenu.classList.toggle('mobile-active');
            mobileToggleBtn.setAttribute('aria-expanded', isOpen ? 'true' : 'false');
        });

        // Close menu on outside click
        document.addEventListener('click', (e) => {
            if (navMenu.classList.contains('mobile-active') && !navMenu.contains(e.target) && !mobileToggleBtn.contains(e.target)) {
                navMenu.classList.remove('mobile-active');
                mobileToggleBtn.setAttribute('aria-expanded', 'false');
            }
        });

        // Close menu on link click
        navMenu.querySelectorAll('.nav-btn').forEach(btn => {
            btn.addEventListener('click', () => {
                navMenu.classList.remove('mobile-active');
                mobileToggleBtn.setAttribute('aria-expanded', 'false');
            });
        });
    }

    // Set active navigation button based on current URL path
    const navButtons = document.querySelectorAll('.nav-btn');
    const currentPath = window.location.pathname.toLowerCase();
    const isHomePage = currentPath.endsWith('/') || currentPath.endsWith('index.html') || currentPath.endsWith('index') || currentPath === '';

    navButtons.forEach(btn => {
        const href = btn.getAttribute('href');
        if (!href) return;

        btn.classList.remove('active');
        const pageFileName = href.split('/').pop().toLowerCase();
        const pageBase = pageFileName.replace(/\.html$/, '');

        if ((pageBase === 'index' || pageBase === '') && isHomePage) {
            btn.classList.add('active');
        } else if (pageBase !== 'index' && pageBase !== '' && !isHomePage && currentPath.includes(pageBase)) {
            btn.classList.add('active');
        }
    });

    // Fade-in elements on scroll
    const observerOptions = {
        root: null,
        rootMargin: '0px',
        threshold: 0.15
    };

    const observer = new IntersectionObserver((entries, obs) => {
        entries.forEach(entry => {
            if (entry.isIntersecting) {
                entry.target.classList.add('visible');
                obs.unobserve(entry.target);
            }
        });
    }, observerOptions);

    const animatedElements = document.querySelectorAll('.hero-content, .hero-visual, .feature-text, .feature-visual');
    animatedElements.forEach(el => {
        el.classList.add('fade-in');
        observer.observe(el);
    });

    // Carousel Logic
    const track = document.querySelector('.carousel-track');
    const slides = Array.from(track?.children || []);
    const nextButton = document.querySelector('.carousel-btn.next');
    const prevButton = document.querySelector('.carousel-btn.prev');
    const dotsNav = document.querySelector('.carousel-indicators');
    const dots = Array.from(dotsNav?.children || []);

    if (track && slides.length > 0) {
        let currentSlideIndex = 0;

        const updateCarousel = (index) => {
            track.style.transform = `translateX(-${index * 100}%)`;
            dots.forEach(dot => dot.classList.remove('active'));
            if (dots[index]) dots[index].classList.add('active');
            currentSlideIndex = index;
        };

        setTimeout(() => updateCarousel(0), 100);
        window.addEventListener('resize', () => updateCarousel(currentSlideIndex));

        nextButton?.addEventListener('click', () => {
            let nextIndex = currentSlideIndex + 1;
            if (nextIndex >= slides.length) nextIndex = 0;
            updateCarousel(nextIndex);
        });

        prevButton?.addEventListener('click', () => {
            let prevIndex = currentSlideIndex - 1;
            if (prevIndex < 0) prevIndex = slides.length - 1;
            updateCarousel(prevIndex);
        });

        dotsNav?.addEventListener('click', e => {
            const targetDot = e.target.closest('.dot');
            if (!targetDot) return;
            const index = dots.findIndex(dot => dot === targetDot);
            updateCarousel(index);
        });

        // Auto-play interval
        let autoPlayInterval = setInterval(() => {
            let nextIndex = currentSlideIndex + 1;
            if (nextIndex >= slides.length) nextIndex = 0;
            updateCarousel(nextIndex);
        }, 5000);

        // Pause on hover
        const carouselContainer = document.querySelector('.carousel-container');
        carouselContainer?.addEventListener('mouseenter', () => clearInterval(autoPlayInterval));
        carouselContainer?.addEventListener('mouseleave', () => {
            autoPlayInterval = setInterval(() => {
                let nextIndex = currentSlideIndex + 1;
                if (nextIndex >= slides.length) nextIndex = 0;
                updateCarousel(nextIndex);
            }, 5000);
        });
    }

    // Lightbox Logic & Drag-to-Pan
    const lightbox = document.getElementById('lightbox');
    const lightboxImg = lightbox?.querySelector('img');
    const lightboxClose = document.getElementById('lightboxClose');
    const clickableImages = document.querySelectorAll('.card-img-wrapper, .mockup-img');

    if (lightbox && lightboxImg) {
        clickableImages.forEach(wrapper => {
            wrapper.addEventListener('click', () => {
                const img = wrapper.tagName === 'IMG' ? wrapper : wrapper.querySelector('img');
                if (img) {
                    lightboxImg.src = img.src;
                    lightbox.classList.add('active');
                    document.documentElement.classList.add('modal-open');
                    document.body.classList.add('modal-open');
                    document.documentElement.style.overflow = 'hidden';
                    document.body.style.overflow = 'hidden';
                }
            });
        });

        const lightboxScroll = document.getElementById('lightboxScroll');
        const lightboxCenterer = document.getElementById('lightboxCenterer');

        const closeLightbox = () => {
            lightbox.classList.remove('active');
            lightboxImg.classList.remove('zoomed');
            lightboxCenterer?.classList.remove('zoomed');
            document.documentElement.classList.remove('modal-open');
            document.body.classList.remove('modal-open');
            document.documentElement.style.overflow = '';
            document.body.style.overflow = '';
        };

        lightboxClose?.addEventListener('click', (e) => {
            e.stopPropagation();
            closeLightbox();
        });

        lightbox.addEventListener('click', (e) => {
            if (e.target === lightbox || e.target === lightboxScroll || e.target === lightboxCenterer) {
                closeLightbox();
            }
        });

        let isDragging = false;
        let isMoved = false;
        let startX = 0, startY = 0;
        let startScrollLeft = 0, startScrollTop = 0;

        lightboxImg.addEventListener('dragstart', (e) => e.preventDefault());

        lightboxImg.addEventListener('mousedown', (e) => {
            if (!lightboxImg.classList.contains('zoomed')) return;
            e.preventDefault();
            isDragging = true;
            isMoved = false;
            startX = e.clientX;
            startY = e.clientY;
            startScrollLeft = lightboxScroll.scrollLeft;
            startScrollTop = lightboxScroll.scrollTop;
            lightboxImg.style.cursor = 'grabbing';
            lightboxImg.style.transition = 'none';
        });

        window.addEventListener('mousemove', (e) => {
            if (!isDragging) return;
            if (Math.abs(e.clientX - startX) > 3 || Math.abs(e.clientY - startY) > 3) {
                isMoved = true;
            }
            lightboxScroll.scrollLeft = startScrollLeft - (e.clientX - startX);
            lightboxScroll.scrollTop = startScrollTop - (e.clientY - startY);
        });

        window.addEventListener('mouseup', () => {
            if (!isDragging) return;
            isDragging = false;
            lightboxImg.style.cursor = '';
            lightboxImg.style.transition = '';
        });

        lightboxImg.addEventListener('click', (e) => {
            e.stopPropagation();
            if (isMoved) {
                isMoved = false;
                return;
            }
            if (lightboxImg.classList.contains('zoomed')) {
                lightboxImg.classList.remove('zoomed');
                lightboxCenterer?.classList.remove('zoomed');
            } else {
                // Calculate relative click position on unzoomed image
                const rect = lightboxImg.getBoundingClientRect();
                const clickRatioX = rect.width > 0 ? (e.clientX - rect.left) / rect.width : 0.5;
                const clickRatioY = rect.height > 0 ? (e.clientY - rect.top) / rect.height : 0.5;

                lightboxImg.classList.add('zoomed');
                lightboxCenterer?.classList.add('zoomed');

                setTimeout(() => {
                    const targetScrollLeft = (clickRatioX * lightboxScroll.scrollWidth) - (lightboxScroll.clientWidth / 2);
                    const targetScrollTop = (clickRatioY * lightboxScroll.scrollHeight) - (lightboxScroll.clientHeight / 2);
                    lightboxScroll.scrollLeft = Math.max(0, targetScrollLeft);
                    lightboxScroll.scrollTop = Math.max(0, targetScrollTop);
                }, 10);
            }
        });
    }

    // Smooth Accordion Animation for Attributions
    const accordions = document.querySelectorAll('.attribution-accordion');
    accordions.forEach(accordion => {
        const summary = accordion.querySelector('.attribution-section-title');
        if (!summary) return;

        let isAnimating = false;

        summary.addEventListener('click', (e) => {
            e.preventDefault();
            if (isAnimating) return;
            isAnimating = true;

            if (accordion.open) {
                const startHeight = accordion.offsetHeight;
                const endHeight = summary.offsetHeight;

                accordion.style.height = `${startHeight}px`;
                accordion.style.overflow = 'hidden';
                accordion.classList.add('is-collapsing');

                requestAnimationFrame(() => {
                    accordion.style.transition = 'height 0.35s cubic-bezier(0.16, 1, 0.3, 1)';
                    accordion.style.height = `${endHeight}px`;
                });

                setTimeout(() => {
                    accordion.open = false;
                    accordion.style.height = '';
                    accordion.style.overflow = '';
                    accordion.style.transition = '';
                    accordion.classList.remove('is-collapsing');
                    isAnimating = false;
                }, 350);
            } else {
                accordion.open = true;
                const endHeight = accordion.offsetHeight;
                const startHeight = summary.offsetHeight;

                accordion.style.height = `${startHeight}px`;
                accordion.style.overflow = 'hidden';

                requestAnimationFrame(() => {
                    accordion.style.transition = 'height 0.35s cubic-bezier(0.16, 1, 0.3, 1)';
                    accordion.style.height = `${endHeight}px`;
                });

                setTimeout(() => {
                    accordion.style.height = '';
                    accordion.style.overflow = '';
                    accordion.style.transition = '';
                    isAnimating = false;
                }, 350);
            }
        });
    });

    // License Modal Logic
    const licenseBtns = document.querySelectorAll('[data-license]');
    const licenseModal = document.getElementById('licenseModal');
    const closeLicenseModal = document.getElementById('closeLicenseModal');
    const licenseTextContent = document.getElementById('licenseTextContent');
    const licenseModalTitle = document.getElementById('licenseModalTitle');

    if (licenseBtns.length > 0 && licenseModal) {
        licenseBtns.forEach(btn => {
            btn.addEventListener('click', async () => {
                const licenseFile = btn.getAttribute('data-license');
                const card = btn.closest('.attribution-card');
                const projectName = card ? card.querySelector('.attribution-title').textContent : 'Track-N-Race';
                
                licenseModalTitle.textContent = `${projectName} License`;
                licenseTextContent.textContent = 'Loading...';
                licenseModal.classList.add('active');
                document.documentElement.classList.add('modal-open');
                document.body.classList.add('modal-open');
                document.documentElement.style.overflow = 'hidden';
                document.body.style.overflow = 'hidden';

                try {
                    const response = await fetch(`assets/licenses/${licenseFile}`);
                    if (!response.ok) throw new Error('Failed to load license');
                    const text = await response.text();
                    licenseTextContent.textContent = text;
                } catch (error) {
                    licenseTextContent.textContent = 'Error loading license text. Please try again later.';
                    console.error(error);
                }
            });
        });

        const closeModal = () => {
            licenseModal.classList.remove('active');
            document.documentElement.classList.remove('modal-open');
            document.body.classList.remove('modal-open');
            document.documentElement.style.overflow = '';
            document.body.style.overflow = '';
        };

        closeLicenseModal?.addEventListener('click', closeModal);

        licenseModal.addEventListener('click', (e) => {
            if (e.target === licenseModal) {
                closeModal();
            }
        });
    }

    // Fetch latest release version for Download button
    const downloadBtn = document.getElementById('downloadBtn');
    if (downloadBtn) {
        const getCookie = (name) => {
            const value = `; ${document.cookie}`;
            const parts = value.split(`; ${name}=`);
            if (parts.length === 2) return parts.pop().split(';').shift();
            return null;
        };

        const cachedVersion = getCookie('tnr_latest_version');

        const btnSpan = downloadBtn.querySelector('span');

        const updateBtnText = (version) => {
            if (btnSpan) {
                btnSpan.textContent = `Download ${version}`;
            } else {
                downloadBtn.textContent = `Download ${version}`;
            }
        };

        if (cachedVersion) {
            updateBtnText(cachedVersion);
        } else {
            fetch('https://api.github.com/repos/NoGoat/Track-N-Race/releases/latest')
                .then(response => response.json())
                .then(data => {
                    if (data && data.tag_name) {
                        updateBtnText(data.tag_name);
                        document.cookie = `tnr_latest_version=${data.tag_name}; max-age=3600; path=/; SameSite=Lax`;
                    }
                })
                .catch(error => {
                    console.error('Error fetching latest release:', error);
                });
        }
    }
});
