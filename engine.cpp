// Compile with:
// g++ -std=c++11 -Wall -g -o missile_simulation missile_simulation.cpp -lallegro -lallegro_primitives -lallegro_font -lallegro_ttf -lpthread

#include <allegro5/allegro.h>
#include <allegro5/allegro_primitives.h>
#include <allegro5/allegro_font.h>
#include <allegro5/allegro_ttf.h>
#include <cmath>
#include <vector>
#include <mutex>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <iostream> // For std::cout and std::cerr
#include <memory>   // For std::shared_ptr and std::weak_ptr

// Constants
const float SCREEN_WIDTH = 800.0f;
const float SCREEN_HEIGHT = 600.0f;
const float FPS = 60.0f;

// Global variables
std::vector<std::shared_ptr<class EnemyTarget>> enemyTargets;
std::vector<std::shared_ptr<class DefenseMissile>> defenseMissiles;
std::mutex dataMutex;

// EnemyTarget class definition
class EnemyTarget {
public:
    EnemyTarget(float x, float y, float speedX, float speedY)
        : x(x), y(y), speedX(speedX), speedY(speedY), id(nextID++) {}

    void update(float deltaTime) {
        x += speedX * deltaTime;
        y += speedY * deltaTime;

        // Remove target if it goes off-screen
        if (x > SCREEN_WIDTH || y < 0 || y > SCREEN_HEIGHT) {
            isActive = false;
        }
    }

    void draw() const {
        al_draw_filled_circle(x, y, 10, al_map_rgb(255, 0, 0));
    }

    float getX() const { return x; }
    float getY() const { return y; }
    float getSpeedX() const { return speedX; }
    float getSpeedY() const { return speedY; }
    bool isActiveTarget() const { return isActive; }
    void setInactive() { isActive = false; }

    int getID() const { return id; }

private:
    float x, y;
    float speedX, speedY;
    bool isActive = true;
    int id;
    static int nextID;
};

int EnemyTarget::nextID = 0;

// DefenseMissile class definition
class DefenseMissile {
public:
    DefenseMissile(float x, float y, std::shared_ptr<EnemyTarget> target, float speed)
        : x(x), y(y), speed(speed), target(target) {
        updateVelocity();
    }

    void update(float deltaTime) {
        if (auto sharedTarget = target.lock()) {
            if (sharedTarget->isActiveTarget()) {
                // Update velocity towards the target's current position
                updateVelocity();
            } else {
                // Target is inactive, missile continues in current direction
                target.reset();
            }
        }

        x += velocityX * deltaTime;
        y += velocityY * deltaTime;

        // Remove missile if it goes off-screen
        if (x < 0 || x > SCREEN_WIDTH || y < 0 || y > SCREEN_HEIGHT) {
            isActive = false;
        }
    }

    void draw() const {
        al_draw_filled_circle(x, y, 5, al_map_rgb(0, 255, 0));
    }

    float getX() const { return x; }
    float getY() const { return y; }
    bool isActiveMissile() const { return isActive; }
    void setInactive() { isActive = false; }

    std::weak_ptr<EnemyTarget> target; // Make target public to access in collision detection

private:
    void updateVelocity() {
        if (auto sharedTarget = target.lock()) {
            float dx = sharedTarget->getX() - x;
            float dy = sharedTarget->getY() - y;
            float distance = std::sqrt(dx * dx + dy * dy);

            if (distance > 0.01f) { // Use a small epsilon to avoid division by zero
                velocityX = (dx / distance) * speed;
                velocityY = (dy / distance) * speed;
            }
        }
    }

    float x, y;
    float speed;
    float velocityX = 0.0f;
    float velocityY = 0.0f;
    bool isActive = true;
};

// Function declarations
void updateEntities(float deltaTime);
void drawEntities();
void launchMissile(float startX, float startY, std::shared_ptr<EnemyTarget> target); // No mutex lock inside
void detectionTask();
void drawDetectionRange();
void drawPredictedTrajectory(const EnemyTarget& target);

int main() {
    // Initialize Allegro
    if (!al_init()) {
        std::cerr << "Failed to initialize Allegro!" << std::endl;
        return -1;
    }

    // Initialize addons
    if (!al_init_primitives_addon()) {
        std::cerr << "Failed to initialize primitives addon!" << std::endl;
        return -1;
    }
    if (!al_install_keyboard()) {
        std::cerr << "Failed to initialize keyboard!" << std::endl;
        return -1;
    }
    if (!al_install_mouse()) {
        std::cerr << "Failed to initialize mouse!" << std::endl;
        return -1;
    }
    al_init_font_addon();
    al_init_ttf_addon();

    // Create display
    ALLEGRO_DISPLAY* display = al_create_display(static_cast<int>(SCREEN_WIDTH), static_cast<int>(SCREEN_HEIGHT));
    if (!display) {
        std::cerr << "Failed to create display!" << std::endl;
        return -1;
    }

    // Load font
    ALLEGRO_FONT* font = al_create_builtin_font();
    if (!font) {
        std::cerr << "Failed to create font!" << std::endl;
        al_destroy_display(display);
        return -1;
    }

    // Create event queue and timer
    ALLEGRO_EVENT_QUEUE* event_queue = al_create_event_queue();
    ALLEGRO_TIMER* timer = al_create_timer(1.0 / FPS);

    // Register event sources
    al_register_event_source(event_queue, al_get_display_event_source(display));
    al_register_event_source(event_queue, al_get_timer_event_source(timer));
    al_register_event_source(event_queue, al_get_keyboard_event_source());
    al_register_event_source(event_queue, al_get_mouse_event_source());

    // Start the timer
    al_start_timer(timer);

    // Initialize random seed
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    // Main loop variables
    bool running = true;
    bool redraw = true;

    // Timers
    float enemySpawnTimer = 0.0f;
    const float enemySpawnInterval = 2.0f; // Spawn every 2 seconds

    float detectionTimer = 0.0f;
    const float detectionInterval = 0.5f; // Check every 0.5 seconds

    while (running) {
        ALLEGRO_EVENT ev;
        al_wait_for_event(event_queue, &ev);

        if (ev.type == ALLEGRO_EVENT_TIMER) {
            // Update simulation
            float deltaTime = 1.0f / FPS;

            // Update enemy spawn timer
            enemySpawnTimer += deltaTime;
            if (enemySpawnTimer >= enemySpawnInterval) {
                enemySpawnTimer -= enemySpawnInterval;

                // Spawn a new enemy target from the left edge
                float startX = 0.0f;
                float startY = static_cast<float>(std::rand() % static_cast<int>(SCREEN_HEIGHT));
                float speedX = 50.0f + static_cast<float>(std::rand() % 50); // Random speed between 50 and 100
                float speedY = 0.0f;

                {
                    std::lock_guard<std::mutex> lock(dataMutex);
                    enemyTargets.emplace_back(std::make_shared<EnemyTarget>(startX, startY, speedX, speedY));
                }
            }

            // Update detection timer
            detectionTimer += deltaTime;
            if (detectionTimer >= detectionInterval) {
                detectionTimer -= detectionInterval;
                detectionTask();
            }

            // Update entities
            updateEntities(deltaTime);

            redraw = true;
        }
        else if (ev.type == ALLEGRO_EVENT_DISPLAY_CLOSE) {
            running = false;
        }
        else if (ev.type == ALLEGRO_EVENT_KEY_DOWN) {
            if (ev.keyboard.keycode == ALLEGRO_KEY_ESCAPE)
                running = false;
            else if (ev.keyboard.keycode == ALLEGRO_KEY_SPACE) {
                // Launch missile toward the first enemy target
                {
                    std::lock_guard<std::mutex> lock(dataMutex);
                    if (!enemyTargets.empty()) {
                        float missileStartX = SCREEN_WIDTH; // Launch from the right edge
                        float missileStartY = SCREEN_HEIGHT / 2.0f; // Middle of the screen
                        auto target = enemyTargets.front();
                        launchMissile(missileStartX, missileStartY, target);
                    }
                }
            }
        }

        if (redraw && al_is_event_queue_empty(event_queue)) {
            redraw = false;

            // Clear screen
            al_clear_to_color(al_map_rgb(0, 0, 0));

            // Draw entities
            drawEntities();

            // Draw HUD
            al_draw_textf(font, al_map_rgb(255, 255, 255), 10, 10, 0, "Targets: %zu", enemyTargets.size());
            al_draw_textf(font, al_map_rgb(255, 255, 255), 10, 30, 0, "Missiles: %zu", defenseMissiles.size());
            al_draw_text(font, al_map_rgb(255, 255, 255), 10, 50, 0, "Press SPACE to manually launch a missile.");

            // Flip display
            al_flip_display();
        }
    }

    // Clean up
    al_destroy_font(font);
    al_destroy_timer(timer);
    al_destroy_event_queue(event_queue);
    al_destroy_display(display);

    return 0;
}

// Function implementations

// Update all entities
void updateEntities(float deltaTime) {
    std::lock_guard<std::mutex> lock(dataMutex);

    // Update enemy targets
    for (auto& target : enemyTargets) {
        target->update(deltaTime);
    }

    // Update defense missiles
    for (auto& missile : defenseMissiles) {
        missile->update(deltaTime);
    }

    // Collision detection: Mark missiles and targets as inactive upon collision
    for (auto& missile : defenseMissiles) {
        if (!missile->isActiveMissile()) continue;

        if (auto targetPtr = missile->target.lock()) {
            if (!targetPtr->isActiveTarget()) continue;

            float dx = missile->getX() - targetPtr->getX();
            float dy = missile->getY() - targetPtr->getY();
            if ((dx * dx + dy * dy) < 225) { // Collision radius of 15 units
                missile->setInactive();
                targetPtr->setInactive();
                // No need to break, as missile can only have one target
            }
        }
    }

    // Remove inactive targets
    enemyTargets.erase(
        std::remove_if(enemyTargets.begin(), enemyTargets.end(),
                       [](const std::shared_ptr<EnemyTarget>& target) { return !target->isActiveTarget(); }),
        enemyTargets.end());

    // Remove inactive missiles
    defenseMissiles.erase(
        std::remove_if(defenseMissiles.begin(), defenseMissiles.end(),
                       [](const std::shared_ptr<DefenseMissile>& missile) { return !missile->isActiveMissile(); }),
        defenseMissiles.end());
}

// Draw all entities
void drawEntities() {
    // Draw detection range
    drawDetectionRange();

    std::lock_guard<std::mutex> lock(dataMutex);

    // Draw enemy targets and their predicted trajectories
    for (const auto& target : enemyTargets) {
        target->draw();
        drawPredictedTrajectory(*target);
    }

    // Draw defense missiles
    for (const auto& missile : defenseMissiles) {
        missile->draw();
    }
}

// Launch a missile towards a target
void launchMissile(float startX, float startY, std::shared_ptr<EnemyTarget> target) {
    // Assume dataMutex is locked by the caller
    float missileSpeed = 200.0f; // Adjust as needed
    defenseMissiles.emplace_back(std::make_shared<DefenseMissile>(startX, startY, target, missileSpeed));
}

// Detection task to detect targets within range and launch missiles
void detectionTask() {
    std::lock_guard<std::mutex> lock(dataMutex);

    float detectionRange = 500.0f; // Adjust as needed
    float sensorX = SCREEN_WIDTH; // Sensor location at the right edge
    float sensorY = SCREEN_HEIGHT / 2.0f;

    for (const auto& target : enemyTargets) {
        float dx = target->getX() - sensorX;
        float dy = target->getY() - sensorY;
        float distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= detectionRange) {
            // Target detected, launch missile
            launchMissile(sensorX, sensorY, target);
            // For simplicity, break after launching at one target
            break;
        }
    }
}

// Draw the detection range
void drawDetectionRange() {
    float sensorX = SCREEN_WIDTH; // Sensor location at the right edge
    float sensorY = SCREEN_HEIGHT / 2.0f;
    float detectionRange = 500.0f;

    al_draw_circle(sensorX, sensorY, detectionRange, al_map_rgb(0, 0, 255), 1);
}

// Draw predicted trajectory of an enemy target
void drawPredictedTrajectory(const EnemyTarget& target) {
    float predictionTime = 5.0f; // Predict 5 seconds into the future
    float deltaTime = 0.5f; // Draw a point every 0.5 seconds
    float currentTime = 0.0f;

    float x = target.getX();
    float y = target.getY();
    float speedX = target.getSpeedX();
    float speedY = target.getSpeedY();

    while (currentTime < predictionTime) {
        x += speedX * deltaTime;
        y += speedY * deltaTime;
        al_draw_filled_circle(x, y, 2, al_map_rgb(255, 255, 0));
        currentTime += deltaTime;
    }
}
